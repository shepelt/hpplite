// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./HPPLite.sol";
import "./HPPLiteDA.sol";

/**
 * @title HPPLiteFactory
 * @notice Factory contract for deploying HPPLite rollup instances
 * @dev Deploys both HPPLite (coordination) and HPPLiteDA (data availability)
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
        address indexed daContract,
        address indexed owner,
        address sequencer,
        uint256 requiredAttestations
    );

    // ============ State ============
    address[] public rollups;
    mapping(address => address) public rollupOf;      // owner => their rollup (1:1)
    mapping(address => address[]) public rollupsByOwner;  // owner => all rollups
    mapping(address => address) public daOf;          // rollup => its DA contract

    // ============ Factory Functions ============

    /**
     * @notice Create a new HPPLite rollup with on-chain DA
     * @param sequencer Address that will sequence batches
     * @param witnesses Array of witness addresses
     * @param requiredAttestations Number of witness signatures needed
     * @param checkpointInterval Batches between checkpoints
     * @param sequencerTimeout Seconds before sequencer can be replaced
     * @return rollup Address of the HPPLite contract
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
            sequencerTimeout,
            "hppda"  // default to on-chain DA
        );
    }

    /**
     * @notice Create rollup with custom DA scheme
     * @param daScheme DA scheme: "hppda" (on-chain), "ipfs", "file"
     */
    function createRollupWithDA(
        address sequencer,
        address[] calldata witnesses,
        uint256 requiredAttestations,
        uint256 checkpointInterval,
        uint256 sequencerTimeout,
        string calldata daScheme
    ) external returns (address rollup) {
        return _createRollupInternal(
            msg.sender,
            sequencer,
            witnesses,
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout,
            daScheme
        );
    }

    /**
     * @dev Internal function to create rollup
     */
    function _createRollupInternal(
        address owner,
        address sequencer,
        address[] memory witnesses,
        uint256 requiredAttestations,
        uint256 checkpointInterval,
        uint256 sequencerTimeout,
        string memory daScheme
    ) internal returns (address rollup) {
        require(sequencer != address(0), "Invalid sequencer");
        require(requiredAttestations <= witnesses.length, "Not enough witnesses");
        require(checkpointInterval > 0, "Invalid checkpoint interval");

        address daContract = address(0);

        // Deploy DA contract if using on-chain DA
        if (keccak256(bytes(daScheme)) == keccak256(bytes("hppda"))) {
            HPPLiteDA da = new HPPLiteDA(
                sequencer,      // sequencer can submit batches
                128 * 1024      // 128KB batch size limit
            );
            daContract = address(da);
        }

        // Deploy HPPLite coordination contract
        HPPLite newRollup = new HPPLite(
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout,
            daScheme,
            daContract
        );

        // Configure the rollup
        newRollup.setSequencer(sequencer);
        for (uint256 i = 0; i < witnesses.length; i++) {
            newRollup.addWitness(witnesses[i]);
        }

        // Transfer ownership
        newRollup.transferOwnership(owner);
        if (daContract != address(0)) {
            HPPLiteDA(daContract).transferOwnership(owner);
        }

        // Track the rollup
        rollup = address(newRollup);
        rollups.push(rollup);
        rollupsByOwner[owner].push(rollup);
        daOf[rollup] = daContract;

        // Set as primary rollup if owner doesn't have one
        if (rollupOf[owner] == address(0)) {
            rollupOf[owner] = rollup;
        }

        emit RollupCreated(rollup, daContract, owner, sequencer, requiredAttestations);
    }

    /**
     * @notice Get or create rollup for caller (1:1 wallet:rollup)
     * @return rollup The caller's rollup (existing or newly created)
     */
    function getOrCreateRollup() external returns (address rollup) {
        rollup = rollupOf[msg.sender];
        if (rollup == address(0)) {
            address[] memory noWitnesses = new address[](0);
            rollup = _createRollupInternal(
                msg.sender,     // owner
                msg.sender,     // caller is sequencer
                noWitnesses,
                0,              // no attestations required
                10,             // default checkpoint interval
                3600,           // 1 hour timeout
                "hppda"         // on-chain DA
            );
        }
    }

    /**
     * @notice Create a minimal rollup (sequencer-only, no witnesses)
     */
    function createSimpleRollup(address sequencer) external returns (address) {
        address[] memory noWitnesses = new address[](0);
        return _createRollupInternal(
            msg.sender,
            sequencer,
            noWitnesses,
            0,
            10,
            3600,
            "hppda"
        );
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
