// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./HPPLite.sol";
import "./HPPLiteDA.sol";

/**
 * @title HPPLiteFactory
 * @notice Factory for deploying HPPLite rollups (singleton sequencer model)
 * @dev Deploys HPPLite (coordination + lease) and HPPLiteDA (data availability)
 *
 * Usage:
 *   address rollup = factory.getOrCreateRollup();
 *   // Then call claimSequencer(instanceId) on the rollup
 */
contract HPPLiteFactory {
    // ============ Events ============
    event RollupCreated(
        address indexed rollup,
        address indexed daContract,
        address indexed owner,
        uint256 checkpointInterval
    );

    // ============ State ============
    address[] public rollups;
    mapping(address => address) public rollupOf;         // owner => their rollup (1:1)
    mapping(address => address[]) public rollupsByOwner; // owner => all rollups
    mapping(address => address) public daOf;             // rollup => its DA contract

    // ============ Factory Functions ============

    /**
     * @notice Create a new HPPLite rollup
     * @param checkpointInterval Batches between checkpoints
     * @return rollup Address of the HPPLite contract
     */
    function createRollup(uint256 checkpointInterval) external returns (address rollup) {
        return _createRollupInternal(msg.sender, checkpointInterval, "hppda");
    }

    /**
     * @notice Create rollup with custom DA scheme
     * @param checkpointInterval Batches between checkpoints
     * @param daScheme DA scheme: "hppda" (on-chain), "ipfs", "file"
     */
    function createRollupWithDA(
        uint256 checkpointInterval,
        string calldata daScheme
    ) external returns (address rollup) {
        return _createRollupInternal(msg.sender, checkpointInterval, daScheme);
    }

    /**
     * @notice Get or create rollup for caller (1:1 wallet:rollup)
     * @return rollup The caller's rollup (existing or newly created)
     */
    function getOrCreateRollup() external returns (address rollup) {
        rollup = rollupOf[msg.sender];
        if (rollup == address(0)) {
            rollup = _createRollupInternal(msg.sender, 100, "hppda");
        }
    }

    /**
     * @dev Internal function to create rollup
     */
    function _createRollupInternal(
        address owner,
        uint256 checkpointInterval,
        string memory daScheme
    ) internal returns (address rollup) {
        require(checkpointInterval > 0, "Invalid checkpoint interval");

        // Deploy HPPLite coordination contract first (DA needs its address)
        HPPLite newRollup = new HPPLite(
            checkpointInterval,
            daScheme,
            address(0)  // Will set DA contract after deployment
        );

        address daContract = address(0);

        // Deploy DA contract if using on-chain DA
        if (keccak256(bytes(daScheme)) == keccak256(bytes("hppda"))) {
            HPPLiteDA da = new HPPLiteDA(
                address(newRollup),  // coordinator for lease verification
                128 * 1024           // 128KB batch size limit
            );
            daContract = address(da);

            // Link DA contract to HPPLite
            newRollup.setDAConfig(daScheme, daContract);

            // Transfer DA ownership
            da.transferOwnership(owner);
        }

        // Transfer HPPLite ownership
        newRollup.transferOwnership(owner);

        // Track the rollup
        rollup = address(newRollup);
        rollups.push(rollup);
        rollupsByOwner[owner].push(rollup);
        daOf[rollup] = daContract;

        // Set as primary rollup if owner doesn't have one
        if (rollupOf[owner] == address(0)) {
            rollupOf[owner] = rollup;
        }

        emit RollupCreated(rollup, daContract, owner, checkpointInterval);
    }

    // ============ View Functions ============

    function getRollup(address owner) external view returns (address) {
        return rollupOf[owner];
    }

    function getDA(address rollup) external view returns (address) {
        return daOf[rollup];
    }

    function hasRollup(address owner) external view returns (bool) {
        return rollupOf[owner] != address(0);
    }

    function getRollupCount() external view returns (uint256) {
        return rollups.length;
    }

    function getRollupsByOwner(address owner) external view returns (address[] memory) {
        return rollupsByOwner[owner];
    }

    function getAllRollups() external view returns (address[] memory) {
        return rollups;
    }
}
