// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Test.sol";
import "../src/HPPLite.sol";

contract HPPLiteTest is Test {
    HPPLite public hpplite;
    
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
        
        // Deploy contract
        hpplite = new HPPLite(
            2,      // requiredAttestations
            10,     // checkpointInterval
            3600    // sequencerTimeout (1 hour)
        );
    }
    
    function test_InitialState() public view {
        assertEq(hpplite.owner(), owner);
        assertEq(hpplite.sequencer(), address(0));
        assertEq(hpplite.requiredAttestations(), 2);
        assertEq(hpplite.checkpointInterval(), 10);
        assertEq(hpplite.sequencerTimeout(), 3600);
    }
    
    function test_SetSequencer() public {
        hpplite.setSequencer(sequencer);
        assertEq(hpplite.sequencer(), sequencer);
    }
    
    function test_SetSequencer_NotOwner() public {
        vm.prank(sequencer);
        vm.expectRevert("not owner");
        hpplite.setSequencer(sequencer);
    }
    
    function test_AddWitness() public {
        hpplite.addWitness(witness1);
        assertTrue(hpplite.witnesses(witness1));
        
        address[] memory witnesses = hpplite.getWitnesses();
        assertEq(witnesses.length, 1);
        assertEq(witnesses[0], witness1);
    }
    
    function test_AddWitness_Duplicate() public {
        hpplite.addWitness(witness1);
        vm.expectRevert("already witness");
        hpplite.addWitness(witness1);
    }
    
    function test_RemoveWitness() public {
        hpplite.addWitness(witness1);
        hpplite.addWitness(witness2);
        
        hpplite.removeWitness(witness1);
        assertFalse(hpplite.witnesses(witness1));
        
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
        assertEq(hpplite.getCheckpointCount(), 1);
    }
    
    function test_SubmitCheckpoint_NotSequencer() public {
        hpplite.setSequencer(sequencer);
        
        vm.expectRevert("not sequencer");
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
        vm.expectRevert("insufficient attestations");
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
    
    function test_SetConfig() public {
        hpplite.setConfig(3, 20, 7200);
        assertEq(hpplite.requiredAttestations(), 3);
        assertEq(hpplite.checkpointInterval(), 20);
        assertEq(hpplite.sequencerTimeout(), 7200);
    }
}
