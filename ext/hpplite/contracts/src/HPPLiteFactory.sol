// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./HPPLiteDA.sol";

/**
 * @title HPPLiteFactory
 * @notice Factory contract for deploying HPPLite rollup instances
 * @dev Deploy this once, then anyone can create their own HPPLite rollup
 *
 * Usage:
 *   address myRollup = factory.createRollup(
 *       mySequencer,
 *       [witness1, witness2],
 *       2,     // required attestations
 *       10,    // checkpoint interval (batches)
 *       3600   // sequencer timeout (seconds)
 *   );
 */
contract HPPLiteFactory {
    // ============ Events ============
    event RollupCreated(
        address indexed rollup,
        address indexed owner,
        address indexed sequencer,
        uint256 requiredAttestations
    );

    // ============ State ============
    address[] public rollups;
    mapping(address => address) public rollupOf;      // owner => their rollup (1:1)
    mapping(address => address[]) public rollupsByOwner;  // owner => all rollups (for multi)

    // ============ Factory Functions ============

    /**
     * @notice Create a new HPPLite rollup instance
     * @param sequencer Address that will sequence batches
     * @param witnesses Array of witness addresses
     * @param requiredAttestations Number of witness signatures needed for checkpoint
     * @param checkpointInterval Batches between checkpoints
     * @param sequencerTimeout Seconds before sequencer can be replaced
     * @return rollup Address of the newly deployed rollup contract
     */
    function createRollup(
        address sequencer,
        address[] calldata witnesses,
        uint256 requiredAttestations,
        uint256 checkpointInterval,
        uint256 sequencerTimeout
    ) external returns (address rollup) {
        return _createRollupInternal(
            msg.sender,
            sequencer,
            witnesses,
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout
        );
    }

    /**
     * @dev Internal function to create rollup with explicit owner
     */
    function _createRollupInternal(
        address owner,
        address sequencer,
        address[] memory witnesses,
        uint256 requiredAttestations,
        uint256 checkpointInterval,
        uint256 sequencerTimeout
    ) internal returns (address rollup) {
        require(sequencer != address(0), "Invalid sequencer");
        require(requiredAttestations <= witnesses.length, "Not enough witnesses");
        require(checkpointInterval > 0, "Invalid checkpoint interval");

        // Deploy new HPPLiteDA contract
        HPPLiteDA newRollup = new HPPLiteDA(
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout
        );

        // Configure the rollup
        newRollup.setSequencer(sequencer);
        for (uint256 i = 0; i < witnesses.length; i++) {
            newRollup.addWitness(witnesses[i]);
        }

        // Transfer ownership to the specified owner
        newRollup.transferOwnership(owner);

        // Track the rollup
        rollup = address(newRollup);
        rollups.push(rollup);
        rollupsByOwner[owner].push(rollup);

        // Set as primary rollup if owner doesn't have one
        if (rollupOf[owner] == address(0)) {
            rollupOf[owner] = rollup;
        }

        emit RollupCreated(rollup, owner, sequencer, requiredAttestations);
    }

    /**
     * @notice Get or create rollup for caller (1:1 wallet:rollup)
     * @dev This is the main entry point - ensures one rollup per wallet
     * @return rollup The caller's rollup (existing or newly created)
     */
    function getOrCreateRollup() external returns (address rollup) {
        rollup = rollupOf[msg.sender];
        if (rollup == address(0)) {
            // Create simple rollup with caller as sequencer
            address[] memory noWitnesses = new address[](0);
            rollup = _createRollupInternal(
                msg.sender,     // owner
                msg.sender,     // caller is sequencer
                noWitnesses,
                0,              // no attestations required
                10,             // default checkpoint interval
                3600            // 1 hour timeout
            );
        }
    }

    /**
     * @notice Create a minimal rollup (sequencer-only, no witnesses)
     * @dev Useful for testing or trusted single-operator setups
     */
    function createSimpleRollup(address sequencer) external returns (address) {
        address[] memory noWitnesses = new address[](0);
        return _createRollupInternal(
            msg.sender,  // owner
            sequencer,
            noWitnesses,
            0,      // no attestations required
            10,     // default checkpoint interval
            3600    // 1 hour timeout
        );
    }

    // ============ View Functions ============

    /**
     * @notice Get rollup for an address (view, no gas)
     * @param owner The wallet address to check
     * @return rollup The rollup address, or 0x0 if none exists
     */
    function getRollup(address owner) external view returns (address) {
        return rollupOf[owner];
    }

    /**
     * @notice Check if an address has a rollup
     */
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
