// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Script.sol";
import "../src/HPPLite.sol";
import "../src/HPPLiteDA.sol";
import "../src/HPPLiteFactory.sol";

contract DeployHPPLite is Script {
    function run() external {
        uint256 deployerPrivateKey = vm.envUint("HPPLITE_PRIVATE_KEY");

        // Config from env or defaults
        uint256 requiredAttestations = vm.envOr("HPPLITE_REQUIRED_ATTESTATIONS", uint256(0));
        uint256 checkpointInterval = vm.envOr("HPPLITE_CHECKPOINT_INTERVAL", uint256(10));
        uint256 sequencerTimeout = vm.envOr("HPPLITE_SEQUENCER_TIMEOUT", uint256(3600));

        vm.startBroadcast(deployerPrivateKey);

        // Deploy DA contract first
        address deployer = vm.addr(deployerPrivateKey);
        HPPLiteDA da = new HPPLiteDA(deployer, 128 * 1024);

        // Deploy main contract with DA reference
        HPPLite hpplite = new HPPLite(
            requiredAttestations,
            checkpointInterval,
            sequencerTimeout,
            "hppda",
            address(da)
        );

        // Set deployer as sequencer
        hpplite.setSequencer(deployer);

        console.log("HPPLiteDA deployed at:", address(da));
        console.log("HPPLite deployed at:", address(hpplite));
        console.log("  Owner:", hpplite.owner());
        console.log("  Sequencer:", hpplite.sequencer());
        console.log("  DA Contract:", address(da));

        vm.stopBroadcast();
    }
}

contract DeployFactory is Script {
    function run() external {
        uint256 deployerPrivateKey = vm.envUint("HPPLITE_PRIVATE_KEY");

        vm.startBroadcast(deployerPrivateKey);

        HPPLiteFactory factory = new HPPLiteFactory();

        console.log("HPPLiteFactory deployed at:", address(factory));

        vm.stopBroadcast();
    }
}
