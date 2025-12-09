// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./HPPLite.sol";

/**
 * @title HPPLiteDA
 * @notice Data Availability contract - batch storage with lease verification
 * @dev Stores batch data on L1 for full reconstructability.
 *      Validates submitter against HPPLite lease to ensure singleton sequencer.
 */
contract HPPLiteDA {
    // ============ State ============
    address public owner;
    HPPLite public coordinator;  // HPPLite contract for lease verification

    // ============ Batch Storage ============
    mapping(uint256 => bytes) public batchData;
    mapping(uint256 => bytes32) public batchHashes;
    uint256 public lastBatchHeight;
    uint256 public batchSizeLimit;

    // ============ Events ============
    event BatchSubmitted(uint256 indexed height, bytes32 indexed batchHash, uint256 dataSize);
    event CoordinatorChanged(address indexed oldCoordinator, address indexed newCoordinator);

    // ============ Modifiers ============
    modifier onlyOwner() {
        require(msg.sender == owner, "Not owner");
        _;
    }

    modifier onlyActiveSequencer(bytes32 instanceId) {
        require(address(coordinator) != address(0), "No coordinator set");
        require(msg.sender == coordinator.sequencerWallet(), "Not sequencer wallet");
        require(instanceId == coordinator.sequencerInstance(), "Wrong instance");
        require(block.timestamp < coordinator.leaseExpiry(), "Lease expired");
        _;
    }

    // ============ Constructor ============
    constructor(address _coordinator, uint256 _batchSizeLimit) {
        owner = msg.sender;
        coordinator = HPPLite(_coordinator);
        batchSizeLimit = _batchSizeLimit > 0 ? _batchSizeLimit : 128 * 1024;
    }

    // ============ Batch Functions ============

    /**
     * @notice Submit batch data for storage
     * @param instanceId Sequencer instance ID (must match lease)
     * @param height Batch height (must be sequential)
     * @param data Batch data (JSON or compressed)
     */
    function submitBatch(
        bytes32 instanceId,
        uint256 height,
        bytes calldata data
    ) external onlyActiveSequencer(instanceId) {
        require(height == lastBatchHeight + 1, "Non-sequential batch");
        require(data.length > 0, "Empty batch");
        require(data.length <= batchSizeLimit, "Batch too large");

        bytes32 batchHash = keccak256(data);
        batchData[height] = data;
        batchHashes[height] = batchHash;
        lastBatchHeight = height;

        emit BatchSubmitted(height, batchHash, data.length);
    }

    /**
     * @notice Submit multiple batches in one transaction
     * @param instanceId Sequencer instance ID (must match lease)
     * @param startHeight Starting height
     * @param batches Array of batch data
     */
    function submitBatches(
        bytes32 instanceId,
        uint256 startHeight,
        bytes[] calldata batches
    ) external onlyActiveSequencer(instanceId) {
        require(startHeight == lastBatchHeight + 1, "Non-sequential batch");

        for (uint256 i = 0; i < batches.length; i++) {
            uint256 height = startHeight + i;
            require(batches[i].length > 0, "Empty batch");
            require(batches[i].length <= batchSizeLimit, "Batch too large");

            bytes32 batchHash = keccak256(batches[i]);
            batchData[height] = batches[i];
            batchHashes[height] = batchHash;

            emit BatchSubmitted(height, batchHash, batches[i].length);
        }

        lastBatchHeight = startHeight + batches.length - 1;
    }

    /**
     * @notice Get batch data by height
     */
    function getBatch(uint256 height) external view returns (bytes memory) {
        return batchData[height];
    }

    /**
     * @notice Get batch hash by height
     */
    function getBatchHash(uint256 height) external view returns (bytes32) {
        return batchHashes[height];
    }

    /**
     * @notice Get range of batch hashes for verification
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

    /**
     * @notice Get DA state summary
     */
    function getDAState() external view returns (
        uint256 _lastBatchHeight,
        uint256 _totalBatches,
        bytes32 _latestBatchHash
    ) {
        return (lastBatchHeight, lastBatchHeight, batchHashes[lastBatchHeight]);
    }

    // ============ Admin Functions ============

    function setCoordinator(address _coordinator) external onlyOwner {
        address old = address(coordinator);
        coordinator = HPPLite(_coordinator);
        emit CoordinatorChanged(old, _coordinator);
    }

    function setBatchSizeLimit(uint256 _limit) external onlyOwner {
        batchSizeLimit = _limit;
    }

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "Invalid owner");
        owner = newOwner;
    }
}
