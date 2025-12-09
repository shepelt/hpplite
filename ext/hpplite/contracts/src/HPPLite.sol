// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title HPPLite
 * @notice HPPLite L2 System Contract - Singleton Sequencer Model
 * @dev Manages sequencer lease, checkpoints, and coordinates with DA contract.
 *      Security model: same as Optimism/Arbitrum - centralized sequencer,
 *      decentralized verification via L1 data availability.
 */
contract HPPLite {
    // ============ Constants ============
    uint256 public constant LEASE_DURATION = 5 minutes;
    uint256 public constant VERSION = 4;

    // ============ State ============
    address public owner;

    // Sequencer lease - ensures singleton even with same private key on multiple instances
    address public sequencerWallet;
    bytes32 public sequencerInstance;  // Unique per process (e.g., hash of pid+timestamp+random)
    uint256 public leaseExpiry;

    // Checkpoints
    uint256 public lastCheckpointHeight;
    bytes32 public lastStateRoot;
    uint256 public lastCheckpointTime;
    uint256 public checkpointInterval;

    // Data Availability
    string public daScheme;
    address public daContract;

    // ============ Events ============
    event SequencerClaimed(address indexed wallet, bytes32 indexed instanceId, uint256 leaseExpiry);
    event LeaseRenewed(address indexed wallet, bytes32 indexed instanceId, uint256 leaseExpiry);
    event CheckpointSubmitted(uint256 indexed fromHeight, uint256 indexed toHeight, bytes32 stateRoot);
    event ConfigUpdated(string indexed key, bytes value);

    // ============ Modifiers ============
    modifier onlyOwner() {
        require(msg.sender == owner, "Not owner");
        _;
    }

    modifier onlyActiveSequencer(bytes32 instanceId) {
        require(msg.sender == sequencerWallet, "Not sequencer wallet");
        require(instanceId == sequencerInstance, "Wrong instance");
        require(block.timestamp < leaseExpiry, "Lease expired");
        _;
    }

    // ============ Constructor ============
    constructor(
        uint256 _checkpointInterval,
        string memory _daScheme,
        address _daContract
    ) {
        owner = msg.sender;
        checkpointInterval = _checkpointInterval > 0 ? _checkpointInterval : 100;
        daScheme = bytes(_daScheme).length > 0 ? _daScheme : "hppda";
        daContract = _daContract;
        lastCheckpointTime = block.timestamp;
    }

    // ============ Sequencer Lease Functions ============

    /**
     * @notice Claim sequencer role (when lease expired or first time)
     * @param instanceId Unique identifier for this process instance
     */
    function claimSequencer(bytes32 instanceId) external {
        require(
            sequencerWallet == address(0) || block.timestamp >= leaseExpiry,
            "Lease still active"
        );
        require(instanceId != bytes32(0), "Invalid instance ID");

        sequencerWallet = msg.sender;
        sequencerInstance = instanceId;
        leaseExpiry = block.timestamp + LEASE_DURATION;

        emit SequencerClaimed(msg.sender, instanceId, leaseExpiry);
    }

    /**
     * @notice Renew lease (must be current sequencer with matching instance)
     * @param instanceId Must match current sequencer instance
     */
    function renewLease(bytes32 instanceId) external onlyActiveSequencer(instanceId) {
        leaseExpiry = block.timestamp + LEASE_DURATION;
        emit LeaseRenewed(msg.sender, instanceId, leaseExpiry);
    }

    /**
     * @notice Check if current sequencer lease is active
     */
    function isLeaseActive() external view returns (bool) {
        return sequencerWallet != address(0) && block.timestamp < leaseExpiry;
    }

    /**
     * @notice Get time remaining on lease (0 if expired)
     */
    function leaseTimeRemaining() external view returns (uint256) {
        if (block.timestamp >= leaseExpiry) return 0;
        return leaseExpiry - block.timestamp;
    }

    // ============ Checkpoint Functions ============

    /**
     * @notice Submit checkpoint (simplified - no attestations in singleton model)
     * @param instanceId Must match current sequencer instance
     * @param fromHeight Start of checkpoint range
     * @param toHeight End of checkpoint range
     * @param stateRoot Post-state root after applying batches
     */
    function submitCheckpoint(
        bytes32 instanceId,
        uint256 fromHeight,
        uint256 toHeight,
        bytes32 stateRoot
    ) external onlyActiveSequencer(instanceId) {
        require(fromHeight == lastCheckpointHeight + 1 || lastCheckpointHeight == 0, "Non-sequential");
        require(toHeight >= fromHeight, "Invalid range");

        lastCheckpointHeight = toHeight;
        lastStateRoot = stateRoot;
        lastCheckpointTime = block.timestamp;

        // Auto-renew lease on activity
        leaseExpiry = block.timestamp + LEASE_DURATION;

        emit CheckpointSubmitted(fromHeight, toHeight, stateRoot);
    }

    // ============ Admin Functions ============

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "Invalid owner");
        owner = newOwner;
    }

    function setDAConfig(string calldata _scheme, address _da) external onlyOwner {
        daScheme = _scheme;
        daContract = _da;
        emit ConfigUpdated("daConfig", abi.encode(_scheme, _da));
    }

    function setCheckpointInterval(uint256 _interval) external onlyOwner {
        checkpointInterval = _interval;
    }

    /**
     * @notice Emergency: force expire lease (owner only)
     * @dev Use if sequencer is misbehaving and needs to be replaced
     */
    function forceExpireLease() external onlyOwner {
        leaseExpiry = block.timestamp;
    }

    // ============ View Functions ============

    function getState() external view returns (
        address _owner,
        address _sequencerWallet,
        bytes32 _sequencerInstance,
        uint256 _leaseExpiry,
        uint256 _lastCheckpointHeight,
        bytes32 _lastStateRoot,
        uint256 _lastCheckpointTime
    ) {
        return (
            owner,
            sequencerWallet,
            sequencerInstance,
            leaseExpiry,
            lastCheckpointHeight,
            lastStateRoot,
            lastCheckpointTime
        );
    }

    function getSystemConfig() external view returns (
        string memory _daScheme,
        address _daContract,
        uint256 _version,
        uint256 _chainId,
        uint256 _checkpointInterval,
        uint256 _leaseDuration
    ) {
        return (daScheme, daContract, VERSION, block.chainid, checkpointInterval, LEASE_DURATION);
    }

    function version() external pure returns (uint256) {
        return VERSION;
    }
}
