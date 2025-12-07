// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Script.sol";
import "../src/HPPLite.sol";

contract DeployHPPLite is Script {
    function run() external {
        uint256 deployerPrivateKey = vm.envUint("HPPLITE_PRIVATE_KEY");
        
        // Config from env or defaults
        uint256 requiredAttestations = vm.envOr("HPPLITE_REQUIRED_ATTESTATIONS", uint256(2));
        uint256 checkpointInterval = vm.envOr("HPPLITE_CHECKPOINT_INTERVAL", uint256(10));
        uint256 sequencerTimeout = vm.envOr("HPPLITE_SEQUENCER_TIMEOUT", uint256(3600));
        
        vm.startBroadcast(deployerPrivateKey);
        
        HPPLite hpplite = new HPPLite(
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout
        );
        
        console.log("HPPLite deployed at:", address(hpplite));
        console.log("  Owner:", hpplite.owner());
        console.log("  Required attestations:", requiredAttestations);
        console.log("  Checkpoint interval:", checkpointInterval);
        console.log("  Sequencer timeout:", sequencerTimeout);
        
        vm.stopBroadcast();
    }
}
