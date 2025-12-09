// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Script.sol";
import "../src/HPPLite.sol";
import "../src/HPPLiteDA.sol";
import "../src/HPPLiteFactory.sol";

contract DeployHPPLite is Script {
    function run() external {
        uint256 deployerPrivateKey = vm.envUint("HPPLITE_PRIVATE_KEY");
        address deployer = vm.addr(deployerPrivateKey);

        uint256 checkpointInterval = vm.envOr("HPPLITE_CHECKPOINT_INTERVAL", uint256(100));

        vm.startBroadcast(deployerPrivateKey);

        // Deploy HPPLite coordination contract first
        HPPLite hpplite = new HPPLite(
            checkpointInterval,
            "hppda",
            address(0)
        );

        // Deploy DA contract linked to HPPLite for lease verification
        HPPLiteDA da = new HPPLiteDA(
            address(hpplite),
            128 * 1024
        );

        // Link DA to HPPLite
        hpplite.setDAConfig("hppda", address(da));

        console.log("HPPLiteDA deployed at:", address(da));
        console.log("HPPLite deployed at:", address(hpplite));
        console.log("  Owner:", hpplite.owner());
        console.log("  DA Contract:", address(da));
        console.log("  Version:", hpplite.version());

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
