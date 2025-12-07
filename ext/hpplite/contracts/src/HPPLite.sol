// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

/**
 * @title HPPLite
 * @notice Owner-controlled L1 anchor for HPPLite rollup
 * @dev Manages sequencer/witness membership and checkpoint submissions
 *
 * Design decisions:
 * - No factory pattern: Direct deployment for PoC simplicity. Factory can be
 *   added later for multi-tenant scenarios (each tenant gets own HPPLite instance).
 * - Owner-controlled: Simple governance for PoC. Production could use multi-sig
 *   or DAO governance for sequencer/witness management.
 */
contract HPPLite {
    // === State ===
    address public owner;
    address public sequencer;
    mapping(address => bool) public witnesses;
    address[] public witnessList;
    
    // === Config ===
    uint256 public requiredAttestations;
    uint256 public checkpointInterval;
    uint256 public sequencerTimeout;
    
    // === Checkpoint State ===
    uint256 public lastCheckpointTime;
    uint256 public lastCheckpointHeight;
    bytes32 public lastStateRoot;
    
    struct Checkpoint {
        uint256 fromHeight;
        uint256 toHeight;
        bytes32 stateRoot;
        uint256 timestamp;
        address submitter;
    }
    
    Checkpoint[] public checkpoints;
    
    // === Events ===
    event SequencerSet(address indexed sequencer);
    event WitnessAdded(address indexed witness);
    event WitnessRemoved(address indexed witness);
    event CheckpointSubmitted(
        uint256 indexed checkpointId,
        uint256 fromHeight,
        uint256 toHeight,
        bytes32 stateRoot
    );
    event ConfigUpdated(
        uint256 requiredAttestations,
        uint256 checkpointInterval,
        uint256 sequencerTimeout
    );
    
    // === Modifiers ===
    modifier onlyOwner() {
        require(msg.sender == owner, "not owner");
        _;
    }
    
    modifier onlySequencer() {
        require(msg.sender == sequencer, "not sequencer");
        _;
    }
    
    // === Constructor ===
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
    }
    
    // === Owner Functions ===
    
    function setSequencer(address _sequencer) external onlyOwner {
        sequencer = _sequencer;
        lastCheckpointTime = block.timestamp;
        emit SequencerSet(_sequencer);
    }
    
    function addWitness(address _witness) external onlyOwner {
        require(!witnesses[_witness], "already witness");
        witnesses[_witness] = true;
        witnessList.push(_witness);
        emit WitnessAdded(_witness);
    }
    
    function removeWitness(address _witness) external onlyOwner {
        require(witnesses[_witness], "not witness");
        witnesses[_witness] = false;
        
        // Remove from list
        for (uint256 i = 0; i < witnessList.length; i++) {
            if (witnessList[i] == _witness) {
                witnessList[i] = witnessList[witnessList.length - 1];
                witnessList.pop();
                break;
            }
        }
        emit WitnessRemoved(_witness);
    }
    
    function setConfig(
        uint256 _requiredAttestations,
        uint256 _checkpointInterval,
        uint256 _sequencerTimeout
    ) external onlyOwner {
        requiredAttestations = _requiredAttestations;
        checkpointInterval = _checkpointInterval;
        sequencerTimeout = _sequencerTimeout;
        emit ConfigUpdated(_requiredAttestations, _checkpointInterval, _sequencerTimeout);
    }
    
    function transferOwnership(address _newOwner) external onlyOwner {
        require(_newOwner != address(0), "zero address");
        owner = _newOwner;
    }
    
    // === Sequencer Functions ===
    
    /**
     * @notice Submit a checkpoint with witness attestations
     * @param fromHeight Starting batch height
     * @param toHeight Ending batch height
     * @param stateRoot Post-state root after applying batches
     * @param attestations Concatenated signatures (65 bytes each: r, s, v)
     */
    function submitCheckpoint(
        uint256 fromHeight,
        uint256 toHeight,
        bytes32 stateRoot,
        bytes calldata attestations
    ) external onlySequencer {
        require(fromHeight == lastCheckpointHeight + 1 || lastCheckpointHeight == 0, 
                "invalid fromHeight");
        require(toHeight >= fromHeight, "invalid range");
        
        // Verify attestations
        bytes32 message = keccak256(abi.encodePacked(fromHeight, toHeight, stateRoot));
        bytes32 ethSignedHash = keccak256(abi.encodePacked(
            "\x19Ethereum Signed Message:\n32", message
        ));
        
        uint256 validAttestations = 0;
        uint256 sigCount = attestations.length / 65;
        
        for (uint256 i = 0; i < sigCount; i++) {
            bytes memory sig = attestations[i * 65:(i + 1) * 65];
            address signer = recoverSigner(ethSignedHash, sig);
            if (witnesses[signer]) {
                validAttestations++;
            }
        }
        
        require(validAttestations >= requiredAttestations, "insufficient attestations");
        
        // Store checkpoint
        checkpoints.push(Checkpoint({
            fromHeight: fromHeight,
            toHeight: toHeight,
            stateRoot: stateRoot,
            timestamp: block.timestamp,
            submitter: msg.sender
        }));
        
        lastCheckpointHeight = toHeight;
        lastStateRoot = stateRoot;
        lastCheckpointTime = block.timestamp;
        
        emit CheckpointSubmitted(checkpoints.length - 1, fromHeight, toHeight, stateRoot);
    }
    
    // === View Functions ===
    
    function getState() external view returns (
        address _owner,
        address _sequencer,
        uint256 _witnessCount,
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
            witnessList.length,
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout,
            lastCheckpointHeight,
            lastStateRoot,
            lastCheckpointTime
        );
    }
    
    function getWitnesses() external view returns (address[] memory) {
        return witnessList;
    }
    
    function getCheckpoint(uint256 id) external view returns (Checkpoint memory) {
        require(id < checkpoints.length, "invalid id");
        return checkpoints[id];
    }
    
    function getCheckpointCount() external view returns (uint256) {
        return checkpoints.length;
    }
    
    function isSequencerTimedOut() external view returns (bool) {
        return block.timestamp > lastCheckpointTime + sequencerTimeout;
    }
    
    // === Internal ===
    
    function recoverSigner(bytes32 hash, bytes memory sig) internal pure returns (address) {
        require(sig.length == 65, "invalid sig length");
        
        bytes32 r;
        bytes32 s;
        uint8 v;
        
        assembly {
            r := mload(add(sig, 32))
            s := mload(add(sig, 64))
            v := byte(0, mload(add(sig, 96)))
        }
        
        if (v < 27) v += 27;
        require(v == 27 || v == 28, "invalid v");
        
        return ecrecover(hash, v, r, s);
    }
}
