// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "forge-std/Script.sol";
import "../src/HPPLiteFactory.sol";

contract DeployFactory is Script {
    function run() external {
        uint256 deployerPrivateKey = vm.envUint("HPPLITE_PRIVATE_KEY");
        address deployer = vm.addr(deployerPrivateKey);

        vm.startBroadcast(deployerPrivateKey);

        HPPLiteFactory factory = new HPPLiteFactory();

        console.log("HPPLiteFactory deployed at:", address(factory));
        console.log("  Deployer:", deployer);   // not msg.sender: newer forge rejects it inside a broadcast

        vm.stopBroadcast();
    }
}
