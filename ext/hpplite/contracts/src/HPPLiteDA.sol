// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title HPPLiteDA
 * @notice Pure Data Availability contract - batch CRUD only
 * @dev Stores batch data on L1 for full reconstructability
 *      Used by HPPLite main contract when daScheme = "hppda"
 */
contract HPPLiteDA {
    // ============ State ============
    address public owner;
    address public submitter;  // Who can submit batches (usually HPPLite contract or sequencer)

    // ============ Batch Storage ============
    mapping(uint256 => bytes) public batchData;      // height => compressed batch JSON
    mapping(uint256 => bytes32) public batchHashes;  // height => keccak256(batchData)
    uint256 public lastBatchHeight;
    uint256 public batchSizeLimit;

    // ============ Events ============
    event BatchSubmitted(
        uint256 indexed height,
        bytes32 indexed batchHash,
        uint256 dataSize
    );
    event SubmitterChanged(address indexed oldSubmitter, address indexed newSubmitter);

    // ============ Modifiers ============
    modifier onlyOwner() {
        require(msg.sender == owner, "Not owner");
        _;
    }

    modifier onlySubmitter() {
        require(msg.sender == submitter || msg.sender == owner, "Not submitter");
        _;
    }

    // ============ Constructor ============
    constructor(address _submitter, uint256 _batchSizeLimit) {
        owner = msg.sender;
        submitter = _submitter;
        batchSizeLimit = _batchSizeLimit > 0 ? _batchSizeLimit : 128 * 1024;  // 128KB default
    }

    // ============ Batch Functions ============

    /**
     * @notice Submit batch data for storage
     * @param height Batch height (must be sequential)
     * @param data Batch data (JSON or compressed)
     */
    function submitBatch(uint256 height, bytes calldata data) external onlySubmitter {
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
     * @param startHeight Starting height
     * @param batches Array of batch data
     */
    function submitBatches(uint256 startHeight, bytes[] calldata batches) external onlySubmitter {
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

    /**
     * @notice Get DA state summary
     * @return _lastBatchHeight Last submitted batch height
     * @return _totalBatches Total number of batches
     * @return _latestBatchHash Hash of the latest batch
     */
    function getDAState() external view returns (
        uint256 _lastBatchHeight,
        uint256 _totalBatches,
        bytes32 _latestBatchHash
    ) {
        return (
            lastBatchHeight,
            lastBatchHeight,  // Sequential from 1
            batchHashes[lastBatchHeight]
        );
    }

    // ============ Admin Functions ============

    function setSubmitter(address _submitter) external onlyOwner {
        address old = submitter;
        submitter = _submitter;
        emit SubmitterChanged(old, _submitter);
    }

    function setBatchSizeLimit(uint256 _limit) external onlyOwner {
        batchSizeLimit = _limit;
    }

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "Invalid owner");
        owner = newOwner;
    }
}
