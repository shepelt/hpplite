// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Test.sol";
import "../src/HPPLite.sol";
import "../src/HPPLiteDA.sol";

contract HPPLiteTest is Test {
    HPPLite public hpplite;
    HPPLiteDA public da;

    address owner = address(this);
    address sequencer;
    uint256 sequencerKey;
    address witness1;
    uint256 witness1Key;
    address witness2;
    uint256 witness2Key;

    function setUp() public {
        // Generate test keys
        sequencerKey = 0x1;
        sequencer = vm.addr(sequencerKey);
        witness1Key = 0x2;
        witness1 = vm.addr(witness1Key);
        witness2Key = 0x3;
        witness2 = vm.addr(witness2Key);

        // Deploy DA contract
        da = new HPPLiteDA(sequencer, 128 * 1024);

        // Deploy main contract
        hpplite = new HPPLite(
            2,              // requiredAttestations
            10,             // checkpointInterval
            3600,           // sequencerTimeout (1 hour)
            "hppda",        // daScheme
            address(da)     // daContract
        );
    }

    function test_InitialState() public view {
        assertEq(hpplite.owner(), owner);
        assertEq(hpplite.sequencer(), address(0));
        assertEq(hpplite.requiredAttestations(), 2);
        assertEq(hpplite.checkpointInterval(), 10);
        assertEq(hpplite.sequencerTimeout(), 3600);
        assertEq(hpplite.daContract(), address(da));
    }

    function test_SetSequencer() public {
        hpplite.setSequencer(sequencer);
        assertEq(hpplite.sequencer(), sequencer);
    }

    function test_SetSequencer_NotOwner() public {
        vm.prank(sequencer);
        vm.expectRevert("Not owner");
        hpplite.setSequencer(sequencer);
    }

    function test_AddWitness() public {
        hpplite.addWitness(witness1);
        assertTrue(hpplite.isWitness(witness1));

        address[] memory witnesses = hpplite.getWitnesses();
        assertEq(witnesses.length, 1);
        assertEq(witnesses[0], witness1);
    }

    function test_AddWitness_Duplicate() public {
        hpplite.addWitness(witness1);
        vm.expectRevert("Already witness");
        hpplite.addWitness(witness1);
    }

    function test_RemoveWitness() public {
        hpplite.addWitness(witness1);
        hpplite.addWitness(witness2);

        hpplite.removeWitness(witness1);
        assertFalse(hpplite.isWitness(witness1));

        address[] memory witnesses = hpplite.getWitnesses();
        assertEq(witnesses.length, 1);
        assertEq(witnesses[0], witness2);
    }

    function test_SubmitCheckpoint() public {
        // Setup
        hpplite.setSequencer(sequencer);
        hpplite.addWitness(witness1);
        hpplite.addWitness(witness2);

        // Create checkpoint data
        uint256 fromHeight = 1;
        uint256 toHeight = 10;
        bytes32 stateRoot = keccak256("state");

        // Create attestations
        bytes32 message = keccak256(abi.encodePacked(fromHeight, toHeight, stateRoot));
        bytes32 ethSignedHash = keccak256(abi.encodePacked(
            "\x19Ethereum Signed Message:\n32", message
        ));

        (uint8 v1, bytes32 r1, bytes32 s1) = vm.sign(witness1Key, ethSignedHash);
        (uint8 v2, bytes32 r2, bytes32 s2) = vm.sign(witness2Key, ethSignedHash);

        bytes memory attestations = abi.encodePacked(r1, s1, v1, r2, s2, v2);

        // Submit
        vm.prank(sequencer);
        hpplite.submitCheckpoint(fromHeight, toHeight, stateRoot, attestations);

        // Verify
        assertEq(hpplite.lastCheckpointHeight(), toHeight);
        assertEq(hpplite.lastStateRoot(), stateRoot);
    }

    function test_SubmitCheckpoint_NotSequencer() public {
        hpplite.setSequencer(sequencer);

        vm.expectRevert("Not sequencer");
        hpplite.submitCheckpoint(1, 10, keccak256("state"), "");
    }

    function test_SubmitCheckpoint_InsufficientAttestations() public {
        hpplite.setSequencer(sequencer);
        hpplite.addWitness(witness1);
        hpplite.addWitness(witness2);

        // Only 1 attestation when 2 required
        uint256 fromHeight = 1;
        uint256 toHeight = 10;
        bytes32 stateRoot = keccak256("state");

        bytes32 message = keccak256(abi.encodePacked(fromHeight, toHeight, stateRoot));
        bytes32 ethSignedHash = keccak256(abi.encodePacked(
            "\x19Ethereum Signed Message:\n32", message
        ));

        (uint8 v1, bytes32 r1, bytes32 s1) = vm.sign(witness1Key, ethSignedHash);
        bytes memory attestations = abi.encodePacked(r1, s1, v1);

        vm.prank(sequencer);
        vm.expectRevert("Insufficient attestations");
        hpplite.submitCheckpoint(fromHeight, toHeight, stateRoot, attestations);
    }

    function test_SequencerTimeout() public {
        hpplite.setSequencer(sequencer);

        assertFalse(hpplite.isSequencerTimedOut());

        // Fast forward past timeout
        vm.warp(block.timestamp + 3601);

        assertTrue(hpplite.isSequencerTimedOut());
    }

    function test_GetState() public {
        hpplite.setSequencer(sequencer);
        hpplite.addWitness(witness1);

        (
            address _owner,
            address _sequencer,
            uint256 _witnessCount,
            uint256 _requiredAttestations,
            uint256 _checkpointInterval,
            uint256 _sequencerTimeout,
            uint256 _lastCheckpointHeight,
            bytes32 _lastStateRoot,
            uint256 _lastCheckpointTime
        ) = hpplite.getState();

        assertEq(_owner, owner);
        assertEq(_sequencer, sequencer);
        assertEq(_witnessCount, 1);
        assertEq(_requiredAttestations, 2);
        assertEq(_checkpointInterval, 10);
        assertEq(_sequencerTimeout, 3600);
        assertEq(_lastCheckpointHeight, 0);
        assertEq(_lastStateRoot, bytes32(0));
    }

    function test_TransferOwnership() public {
        address newOwner = address(0x123);
        hpplite.transferOwnership(newOwner);
        assertEq(hpplite.owner(), newOwner);
    }

    function test_GetSystemConfig() public view {
        (
            string memory daScheme,
            address daContract,
            uint256 version,
            uint256 chainId
        ) = hpplite.getSystemConfig();

        assertEq(daScheme, "hppda");
        assertEq(daContract, address(da));
        assertEq(version, 3);
    }

    function test_PeerDiscovery() public {
        hpplite.setSequencer(sequencer);
        hpplite.addWitness(witness1);

        // Sequencer sets endpoint
        vm.prank(sequencer);
        hpplite.setEndpoint("tcp://127.0.0.1:5555", 0x00010000);

        // Verify endpoint
        (string memory ep, uint32 ver) = hpplite.getSequencerEndpoint();
        assertEq(ep, "tcp://127.0.0.1:5555");
        assertEq(ver, 0x00010000);

        // Witness sets endpoint
        vm.prank(witness1);
        hpplite.setEndpoint("tcp://127.0.0.1:5556", 0x00010000);

        // Get witness endpoints
        (address[] memory addrs, string[] memory eps, uint32[] memory versions) = hpplite.getWitnessEndpoints();
        assertEq(addrs.length, 1);
        assertEq(addrs[0], witness1);
        assertEq(eps[0], "tcp://127.0.0.1:5556");
    }
}

contract HPPLiteDATest is Test {
    HPPLiteDA public da;
    address submitter = address(0x1);

    function setUp() public {
        da = new HPPLiteDA(submitter, 128 * 1024);
    }

    function test_SubmitBatch() public {
        bytes memory batchData = '{"ops":[{"type":"insert"}]}';

        vm.prank(submitter);
        da.submitBatch(1, batchData);

        assertEq(da.lastBatchHeight(), 1);
        assertEq(da.getBatch(1), batchData);
    }

    function test_SubmitBatch_NotSubmitter() public {
        address random = address(0x999);
        vm.prank(random);
        vm.expectRevert("Not submitter");
        da.submitBatch(1, "data");
    }

    function test_SubmitBatch_NonSequential() public {
        vm.prank(submitter);
        vm.expectRevert("Non-sequential batch");
        da.submitBatch(5, "data");  // Should be 1, not 5
    }

    function test_GetDAState() public {
        bytes memory batch1 = "batch1";
        bytes memory batch2 = "batch2";

        vm.startPrank(submitter);
        da.submitBatch(1, batch1);
        da.submitBatch(2, batch2);
        vm.stopPrank();

        (uint256 lastHeight, uint256 total, bytes32 latestHash) = da.getDAState();
        assertEq(lastHeight, 2);
        assertEq(total, 2);
        assertEq(latestHash, keccak256(batch2));
    }
}
