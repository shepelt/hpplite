// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title HPPLiteDA
 * @notice HPPLite L2 System Contract with on-chain Data Availability
 * @dev Stores batch data on L1 for full reconstructability
 */
contract HPPLiteDA {
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

    // ============ Data Availability ============
    // Batch data stored on-chain
    mapping(uint256 => bytes) public batchData;      // height => compressed batch JSON
    mapping(uint256 => bytes32) public batchHashes;  // height => keccak256(batchData)
    uint256 public lastBatchHeight;

    // ============ System Config (Optimism-style) ============
    string public daScheme;                          // URI scheme: "hppda", "ipfs", "file"
    address public daContract;                       // External DA contract (0x0 = self)
    uint256 public batchSizeLimit;                   // Max batch size in bytes
    uint256 public version;                          // Contract version

    // ============ Events ============
    event CheckpointSubmitted(
        uint256 indexed fromHeight,
        uint256 indexed toHeight,
        bytes32 stateRoot
    );

    event BatchSubmitted(
        uint256 indexed height,
        bytes32 indexed batchHash,
        uint256 dataSize
    );

    event SequencerChanged(address indexed oldSequencer, address indexed newSequencer);
    event WitnessAdded(address indexed witness);
    event WitnessRemoved(address indexed witness);
    event ConfigUpdated(string indexed key, bytes value);

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
        uint256 _sequencerTimeout
    ) {
        owner = msg.sender;
        requiredAttestations = _requiredAttestations;
        checkpointInterval = _checkpointInterval;
        sequencerTimeout = _sequencerTimeout;
        lastCheckpointTime = block.timestamp;

        // System config defaults
        daScheme = "hppda";
        daContract = address(this);  // Self-hosted DA
        batchSizeLimit = 128 * 1024; // 128KB
        version = 2;
    }

    // ============ Data Availability Functions ============

    /**
     * @notice Submit batch data to L1 for data availability
     * @param height Batch height (must be sequential)
     * @param data Batch data (JSON or compressed)
     */
    function submitBatch(uint256 height, bytes calldata data) external onlySequencer {
        require(height == lastBatchHeight + 1, "Non-sequential batch");
        require(data.length > 0, "Empty batch");
        require(data.length <= 128 * 1024, "Batch too large"); // 128KB limit

        bytes32 batchHash = keccak256(data);
        batchData[height] = data;
        batchHashes[height] = batchHash;
        lastBatchHeight = height;

        emit BatchSubmitted(height, batchHash, data.length);
    }

    /**
     * @notice Submit multiple batches in one transaction
     * @param startHeight Starting height
     * @param batches Array of batch data
     */
    function submitBatches(uint256 startHeight, bytes[] calldata batches) external onlySequencer {
        require(startHeight == lastBatchHeight + 1, "Non-sequential batch");

        for (uint256 i = 0; i < batches.length; i++) {
            uint256 height = startHeight + i;
            require(batches[i].length > 0, "Empty batch");
            require(batches[i].length <= 128 * 1024, "Batch too large");

            bytes32 batchHash = keccak256(batches[i]);
            batchData[height] = batches[i];
            batchHashes[height] = batchHash;

            emit BatchSubmitted(height, batchHash, batches[i].length);
        }

        lastBatchHeight = startHeight + batches.length - 1;
    }

    /**
     * @notice Get batch data by height
     * @param height Batch height
     * @return data Batch data bytes
     */
    function getBatch(uint256 height) external view returns (bytes memory) {
        return batchData[height];
    }

    /**
     * @notice Get batch hash by height
     * @param height Batch height
     * @return hash Batch data hash
     */
    function getBatchHash(uint256 height) external view returns (bytes32) {
        return batchHashes[height];
    }

    /**
     * @notice Get range of batch hashes for verification
     * @param fromHeight Start height (inclusive)
     * @param toHeight End height (inclusive)
     * @return hashes Array of batch hashes
     */
    function getBatchHashes(uint256 fromHeight, uint256 toHeight)
        external
        view
        returns (bytes32[] memory)
    {
        require(toHeight >= fromHeight, "Invalid range");
        uint256 count = toHeight - fromHeight + 1;
        bytes32[] memory hashes = new bytes32[](count);

        for (uint256 i = 0; i < count; i++) {
            hashes[i] = batchHashes[fromHeight + i];
        }

        return hashes;
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
        require(toHeight <= lastBatchHeight, "Batches not yet submitted");

        // Verify we have required attestations
        uint256 sigCount = signatures.length / 65;
        require(sigCount >= requiredAttestations, "Insufficient attestations");

        // Compute message hash (must match client-side)
        bytes32 message = keccak256(abi.encodePacked(fromHeight, toHeight, stateRoot));
        bytes32 ethSignedHash = keccak256(abi.encodePacked(
            "\x19Ethereum Signed Message:\n32",
            message
        ));

        // Verify each signature
        uint256 validCount = 0;
        address[] memory signers = new address[](sigCount);

        for (uint256 i = 0; i < sigCount; i++) {
            (bytes32 r, bytes32 s, uint8 v) = _splitSignature(signatures, i);
            address signer = ecrecover(ethSignedHash, v, r, s);

            // Check signer is a witness and hasn't signed twice
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

        // Remove from array
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

    // ============ System Config Functions ============

    function setDAScheme(string calldata _scheme) external onlyOwner {
        daScheme = _scheme;
        emit ConfigUpdated("daScheme", bytes(_scheme));
    }

    function setDAContract(address _da) external onlyOwner {
        daContract = _da;
        emit ConfigUpdated("daContract", abi.encode(_da));
    }

    function setBatchSizeLimit(uint256 _limit) external onlyOwner {
        batchSizeLimit = _limit;
        emit ConfigUpdated("batchSizeLimit", abi.encode(_limit));
    }

    /**
     * @notice Get full system config for client bootstrapping
     * @return _daScheme DA URI scheme (e.g., "hppda", "ipfs")
     * @return _daContract DA contract address (address(this) if self-hosted)
     * @return _batchSizeLimit Maximum batch size in bytes
     * @return _version Contract version
     * @return _chainId Current chain ID
     */
    function getSystemConfig() external view returns (
        string memory _daScheme,
        address _daContract,
        uint256 _batchSizeLimit,
        uint256 _version,
        uint256 _chainId
    ) {
        return (
            daScheme,
            daContract,
            batchSizeLimit,
            version,
            block.chainid
        );
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

    function getDAState() external view returns (
        uint256 _lastBatchHeight,
        uint256 totalBatches,
        bytes32 latestBatchHash
    ) {
        return (
            lastBatchHeight,
            lastBatchHeight,  // Same for now (sequential from 1)
            batchHashes[lastBatchHeight]
        );
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
