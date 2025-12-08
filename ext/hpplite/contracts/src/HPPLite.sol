// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title HPPLite
 * @notice HPPLite L2 System Contract - Coordination layer
 * @dev Manages sequencer/witnesses, checkpoints, peer discovery, config
 *      DA is pluggable via daScheme/daContract
 */
contract HPPLite {
    // ============ State ============
    address public owner;
    address public sequencer;
    address[] public witnesses;
    mapping(address => bool) public isWitness;

    uint256 public requiredAttestations;
    uint256 public checkpointInterval;
    uint256 public sequencerTimeout;

    uint256 public lastCheckpointHeight;
    bytes32 public lastStateRoot;
    uint256 public lastCheckpointTime;

    // ============ Data Availability Config ============
    string public daScheme;        // "hppda" (on-chain), "ipfs", "file", etc.
    address public daContract;     // DA contract address (for "hppda" scheme)
    uint256 public version;

    // ============ Peer Discovery ============
    mapping(address => string) public endpoints;      // wallet => "tcp://host:port"
    mapping(address => uint32) public nodeVersions;   // wallet => protocol version

    // ============ Events ============
    event CheckpointSubmitted(
        uint256 indexed fromHeight,
        uint256 indexed toHeight,
        bytes32 stateRoot
    );
    event SequencerChanged(address indexed oldSequencer, address indexed newSequencer);
    event WitnessAdded(address indexed witness);
    event WitnessRemoved(address indexed witness);
    event ConfigUpdated(string indexed key, bytes value);
    event EndpointUpdated(address indexed node, string endpoint, uint32 version);

    // ============ Modifiers ============
    modifier onlyOwner() {
        require(msg.sender == owner, "Not owner");
        _;
    }

    modifier onlySequencer() {
        require(msg.sender == sequencer, "Not sequencer");
        _;
    }

    // ============ Constructor ============
    constructor(
        uint256 _requiredAttestations,
        uint256 _checkpointInterval,
        uint256 _sequencerTimeout,
        string memory _daScheme,
        address _daContract
    ) {
        owner = msg.sender;
        requiredAttestations = _requiredAttestations;
        checkpointInterval = _checkpointInterval;
        sequencerTimeout = _sequencerTimeout;
        lastCheckpointTime = block.timestamp;

        daScheme = bytes(_daScheme).length > 0 ? _daScheme : "hppda";
        daContract = _daContract;
        version = 3;
    }

    // ============ Checkpoint Functions ============

    /**
     * @notice Submit checkpoint with witness attestations
     * @param fromHeight Start of checkpoint range
     * @param toHeight End of checkpoint range
     * @param stateRoot Post-state root after applying batches
     * @param signatures Packed witness signatures (r, s, v) * n
     */
    function submitCheckpoint(
        uint256 fromHeight,
        uint256 toHeight,
        bytes32 stateRoot,
        bytes calldata signatures
    ) external onlySequencer {
        require(fromHeight == lastCheckpointHeight + 1, "Non-sequential checkpoint");
        require(toHeight >= fromHeight, "Invalid range");

        // Verify we have required attestations
        uint256 sigCount = signatures.length / 65;
        require(sigCount >= requiredAttestations, "Insufficient attestations");

        // Compute message hash
        bytes32 message = keccak256(abi.encodePacked(fromHeight, toHeight, stateRoot));
        bytes32 ethSignedHash = keccak256(abi.encodePacked(
            "\x19Ethereum Signed Message:\n32",
            message
        ));

        // Verify signatures
        uint256 validCount = 0;
        address[] memory signers = new address[](sigCount);

        for (uint256 i = 0; i < sigCount; i++) {
            (bytes32 r, bytes32 s, uint8 v) = _splitSignature(signatures, i);
            address signer = ecrecover(ethSignedHash, v, r, s);

            if (isWitness[signer]) {
                bool duplicate = false;
                for (uint256 j = 0; j < validCount; j++) {
                    if (signers[j] == signer) {
                        duplicate = true;
                        break;
                    }
                }
                if (!duplicate) {
                    signers[validCount] = signer;
                    validCount++;
                }
            }
        }

        require(validCount >= requiredAttestations, "Not enough valid attestations");

        // Update state
        lastCheckpointHeight = toHeight;
        lastStateRoot = stateRoot;
        lastCheckpointTime = block.timestamp;

        emit CheckpointSubmitted(fromHeight, toHeight, stateRoot);
    }

    // ============ Peer Discovery Functions ============

    /**
     * @notice Register or update node endpoint
     * @param endpoint ZMQ address (e.g., "tcp://1.2.3.4:5555")
     * @param nodeVersion Protocol version
     */
    function setEndpoint(string calldata endpoint, uint32 nodeVersion) external {
        require(
            msg.sender == sequencer || isWitness[msg.sender],
            "Not sequencer or witness"
        );
        require(bytes(endpoint).length > 0, "Empty endpoint");
        require(bytes(endpoint).length <= 256, "Endpoint too long");

        endpoints[msg.sender] = endpoint;
        nodeVersions[msg.sender] = nodeVersion;

        emit EndpointUpdated(msg.sender, endpoint, nodeVersion);
    }

    /**
     * @notice Get sequencer's endpoint for witness connections
     */
    function getSequencerEndpoint() external view returns (
        string memory endpoint,
        uint32 nodeVersion
    ) {
        return (endpoints[sequencer], nodeVersions[sequencer]);
    }

    /**
     * @notice Get all witness endpoints
     */
    function getWitnessEndpoints() external view returns (
        address[] memory addrs,
        string[] memory eps,
        uint32[] memory versions
    ) {
        uint256 count = witnesses.length;
        addrs = new address[](count);
        eps = new string[](count);
        versions = new uint32[](count);

        for (uint256 i = 0; i < count; i++) {
            addrs[i] = witnesses[i];
            eps[i] = endpoints[witnesses[i]];
            versions[i] = nodeVersions[witnesses[i]];
        }

        return (addrs, eps, versions);
    }

    // ============ Admin Functions ============

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "Invalid owner");
        owner = newOwner;
    }

    function setSequencer(address _sequencer) external onlyOwner {
        address old = sequencer;
        sequencer = _sequencer;
        emit SequencerChanged(old, _sequencer);
    }

    function addWitness(address _witness) external onlyOwner {
        require(!isWitness[_witness], "Already witness");
        witnesses.push(_witness);
        isWitness[_witness] = true;
        emit WitnessAdded(_witness);
    }

    function removeWitness(address _witness) external onlyOwner {
        require(isWitness[_witness], "Not witness");
        isWitness[_witness] = false;

        for (uint256 i = 0; i < witnesses.length; i++) {
            if (witnesses[i] == _witness) {
                witnesses[i] = witnesses[witnesses.length - 1];
                witnesses.pop();
                break;
            }
        }

        emit WitnessRemoved(_witness);
    }

    function setRequiredAttestations(uint256 _required) external onlyOwner {
        require(_required > 0 && _required <= witnesses.length, "Invalid count");
        requiredAttestations = _required;
    }

    function setDAConfig(string calldata _scheme, address _da) external onlyOwner {
        daScheme = _scheme;
        daContract = _da;
        emit ConfigUpdated("daConfig", abi.encode(_scheme, _da));
    }

    // ============ View Functions ============

    function getState() external view returns (
        address _owner,
        address _sequencer,
        uint256 witnessCount,
        uint256 _requiredAttestations,
        uint256 _checkpointInterval,
        uint256 _sequencerTimeout,
        uint256 _lastCheckpointHeight,
        bytes32 _lastStateRoot,
        uint256 _lastCheckpointTime
    ) {
        return (
            owner,
            sequencer,
            witnesses.length,
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout,
            lastCheckpointHeight,
            lastStateRoot,
            lastCheckpointTime
        );
    }

    function getSystemConfig() external view returns (
        string memory _daScheme,
        address _daContract,
        uint256 _version,
        uint256 _chainId
    ) {
        return (daScheme, daContract, version, block.chainid);
    }

    function getWitnesses() external view returns (address[] memory) {
        return witnesses;
    }

    function isSequencerTimedOut() external view returns (bool) {
        return block.timestamp > lastCheckpointTime + sequencerTimeout;
    }

    // ============ Internal Functions ============

    function _splitSignature(bytes calldata signatures, uint256 index)
        internal
        pure
        returns (bytes32 r, bytes32 s, uint8 v)
    {
        uint256 offset = index * 65;
        require(signatures.length >= offset + 65, "Invalid signature length");

        assembly {
            r := calldataload(add(signatures.offset, offset))
            s := calldataload(add(signatures.offset, add(offset, 32)))
            v := byte(0, calldataload(add(signatures.offset, add(offset, 64))))
        }
    }
}
