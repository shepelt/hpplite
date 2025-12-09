// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Test.sol";
import "../src/HPPLite.sol";
import "../src/HPPLiteDA.sol";
import "../src/HPPLiteFactory.sol";

contract HPPLiteTest is Test {
    HPPLite public hpplite;
    HPPLiteDA public da;

    address owner = address(this);
    address sequencer;
    uint256 sequencerKey;

    bytes32 instanceId1 = keccak256("instance1");
    bytes32 instanceId2 = keccak256("instance2");

    function setUp() public {
        sequencerKey = 0x1;
        sequencer = vm.addr(sequencerKey);

        // Deploy HPPLite first
        hpplite = new HPPLite(
            100,            // checkpointInterval
            "hppda",        // daScheme
            address(0)      // daContract (set later)
        );

        // Deploy DA contract with HPPLite as coordinator
        da = new HPPLiteDA(address(hpplite), 128 * 1024);

        // Link DA to HPPLite
        hpplite.setDAConfig("hppda", address(da));
    }

    function test_InitialState() public view {
        assertEq(hpplite.owner(), owner);
        assertEq(hpplite.sequencerWallet(), address(0));
        assertEq(hpplite.sequencerInstance(), bytes32(0));
        assertEq(hpplite.checkpointInterval(), 100);
        assertEq(hpplite.daContract(), address(da));
        assertEq(hpplite.VERSION(), 4);
    }

    function test_ClaimSequencer() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        assertEq(hpplite.sequencerWallet(), sequencer);
        assertEq(hpplite.sequencerInstance(), instanceId1);
        assertTrue(hpplite.isLeaseActive());
    }

    function test_ClaimSequencer_LeaseActive() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        // Another instance tries to claim while lease active
        address other = address(0x999);
        vm.prank(other);
        vm.expectRevert("Lease still active");
        hpplite.claimSequencer(instanceId2);
    }

    function test_ClaimSequencer_AfterLeaseExpires() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        // Fast forward past lease duration
        vm.warp(block.timestamp + 6 minutes);
        assertFalse(hpplite.isLeaseActive());

        // Now another can claim
        address newSequencer = address(0x999);
        vm.prank(newSequencer);
        hpplite.claimSequencer(instanceId2);

        assertEq(hpplite.sequencerWallet(), newSequencer);
        assertEq(hpplite.sequencerInstance(), instanceId2);
    }

    function test_RenewLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        uint256 originalExpiry = hpplite.leaseExpiry();

        // Fast forward 2 minutes
        vm.warp(block.timestamp + 2 minutes);

        vm.prank(sequencer);
        hpplite.renewLease(instanceId1);

        assertGt(hpplite.leaseExpiry(), originalExpiry);
    }

    function test_RenewLease_WrongInstance() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        vm.prank(sequencer);
        vm.expectRevert("Wrong instance");
        hpplite.renewLease(instanceId2);
    }

    function test_SubmitCheckpoint() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        bytes32 stateRoot = keccak256("state");

        vm.prank(sequencer);
        hpplite.submitCheckpoint(instanceId1, 1, 10, stateRoot);

        assertEq(hpplite.lastCheckpointHeight(), 10);
        assertEq(hpplite.lastStateRoot(), stateRoot);
    }

    function test_SubmitCheckpoint_AutoRenewsLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        // Fast forward 4 minutes (almost expired)
        vm.warp(block.timestamp + 4 minutes);

        vm.prank(sequencer);
        hpplite.submitCheckpoint(instanceId1, 1, 10, keccak256("state"));

        // Lease should be renewed
        assertTrue(hpplite.isLeaseActive());
        assertGt(hpplite.leaseTimeRemaining(), 4 minutes);
    }

    function test_SubmitCheckpoint_LeaseExpired() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        // Fast forward past lease
        vm.warp(block.timestamp + 6 minutes);

        vm.prank(sequencer);
        vm.expectRevert("Lease expired");
        hpplite.submitCheckpoint(instanceId1, 1, 10, keccak256("state"));
    }

    function test_SubmitCheckpoint_WrongInstance() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        vm.prank(sequencer);
        vm.expectRevert("Wrong instance");
        hpplite.submitCheckpoint(instanceId2, 1, 10, keccak256("state"));
    }

    function test_ForceExpireLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        assertTrue(hpplite.isLeaseActive());

        // Owner can force expire
        hpplite.forceExpireLease();

        assertFalse(hpplite.isLeaseActive());
    }

    function test_GetState() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        (
            address _owner,
            address _sequencerWallet,
            bytes32 _sequencerInstance,
            uint256 _leaseExpiry,
            uint256 _lastCheckpointHeight,
            bytes32 _lastStateRoot,
            uint256 _lastCheckpointTime
        ) = hpplite.getState();

        assertEq(_owner, owner);
        assertEq(_sequencerWallet, sequencer);
        assertEq(_sequencerInstance, instanceId1);
        assertGt(_leaseExpiry, block.timestamp);
        assertEq(_lastCheckpointHeight, 0);
    }

    function test_GetSystemConfig() public view {
        (
            string memory daScheme,
            address daContract,
            uint256 version,
            uint256 chainId,
            uint256 checkpointInterval,
            uint256 leaseDuration
        ) = hpplite.getSystemConfig();

        assertEq(daScheme, "hppda");
        assertEq(daContract, address(da));
        assertEq(version, 4);
        assertEq(checkpointInterval, 100);
        assertEq(leaseDuration, 5 minutes);
    }

    function test_TransferOwnership() public {
        address newOwner = address(0x123);
        hpplite.transferOwnership(newOwner);
        assertEq(hpplite.owner(), newOwner);
    }
}

contract HPPLiteDATest is Test {
    HPPLite public hpplite;
    HPPLiteDA public da;

    address sequencer;
    bytes32 instanceId = keccak256("instance1");

    function setUp() public {
        sequencer = address(0x1);

        hpplite = new HPPLite(100, "hppda", address(0));
        da = new HPPLiteDA(address(hpplite), 128 * 1024);
        hpplite.setDAConfig("hppda", address(da));

        // Claim sequencer
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId);
    }

    function test_SubmitBatch() public {
        bytes memory batchData = '{"ops":[{"type":"insert"}]}';

        vm.prank(sequencer);
        da.submitBatch(instanceId, 1, batchData);

        assertEq(da.lastBatchHeight(), 1);
        assertEq(da.getBatch(1), batchData);
    }

    function test_SubmitBatch_WrongInstance() public {
        bytes32 wrongInstance = keccak256("wrong");

        vm.prank(sequencer);
        vm.expectRevert("Wrong instance");
        da.submitBatch(wrongInstance, 1, "data");
    }

    function test_SubmitBatch_NotSequencer() public {
        address random = address(0x999);
        vm.prank(random);
        vm.expectRevert("Not sequencer wallet");
        da.submitBatch(instanceId, 1, "data");
    }

    function test_SubmitBatch_NonSequential() public {
        vm.prank(sequencer);
        vm.expectRevert("Non-sequential batch");
        da.submitBatch(instanceId, 5, "data");
    }

    function test_SubmitBatch_LeaseExpired() public {
        // Fast forward past lease
        vm.warp(block.timestamp + 6 minutes);

        vm.prank(sequencer);
        vm.expectRevert("Lease expired");
        da.submitBatch(instanceId, 1, "data");
    }

    function test_SubmitBatches() public {
        bytes[] memory batches = new bytes[](3);
        batches[0] = "batch1";
        batches[1] = "batch2";
        batches[2] = "batch3";

        vm.prank(sequencer);
        da.submitBatches(instanceId, 1, batches);

        assertEq(da.lastBatchHeight(), 3);
        assertEq(da.getBatch(1), "batch1");
        assertEq(da.getBatch(2), "batch2");
        assertEq(da.getBatch(3), "batch3");
    }

    function test_GetDAState() public {
        vm.startPrank(sequencer);
        da.submitBatch(instanceId, 1, "batch1");
        da.submitBatch(instanceId, 2, "batch2");
        vm.stopPrank();

        (uint256 lastHeight, uint256 total, bytes32 latestHash) = da.getDAState();
        assertEq(lastHeight, 2);
        assertEq(total, 2);
        assertEq(latestHash, keccak256("batch2"));
    }

    function test_GetBatchHashes() public {
        vm.startPrank(sequencer);
        da.submitBatch(instanceId, 1, "batch1");
        da.submitBatch(instanceId, 2, "batch2");
        da.submitBatch(instanceId, 3, "batch3");
        vm.stopPrank();

        bytes32[] memory hashes = da.getBatchHashes(1, 3);
        assertEq(hashes.length, 3);
        assertEq(hashes[0], keccak256("batch1"));
        assertEq(hashes[1], keccak256("batch2"));
        assertEq(hashes[2], keccak256("batch3"));
    }
}

contract HPPLiteFactoryTest is Test {
    HPPLiteFactory public factory;

    function setUp() public {
        factory = new HPPLiteFactory();
    }

    function test_CreateRollup() public {
        address rollup = factory.createRollup(100);

        assertTrue(rollup != address(0));
        assertEq(factory.getRollup(address(this)), rollup);
        assertTrue(factory.hasRollup(address(this)));
    }

    function test_GetOrCreateRollup() public {
        // First call creates
        address rollup1 = factory.getOrCreateRollup();
        assertTrue(rollup1 != address(0));

        // Second call returns existing
        address rollup2 = factory.getOrCreateRollup();
        assertEq(rollup1, rollup2);
    }

    function test_FactoryCreatesLinkedContracts() public {
        address rollup = factory.createRollup(50);
        address da = factory.getDA(rollup);

        HPPLite hpplite = HPPLite(rollup);
        HPPLiteDA daContract = HPPLiteDA(da);

        // Verify linkage
        assertEq(hpplite.daContract(), da);
        assertEq(address(daContract.coordinator()), rollup);
    }

    function test_OwnershipTransferred() public {
        address rollup = factory.createRollup(100);
        address da = factory.getDA(rollup);

        assertEq(HPPLite(rollup).owner(), address(this));
        assertEq(HPPLiteDA(da).owner(), address(this));
    }
}
