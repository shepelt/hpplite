#!/bin/sh
# Real-contract tests on a local chain (issue #47): boot anvil, deploy the
# factory with forge, run test_anvil (real-L1 build) against it, shut down.
#   tests/l1/anvil_test.sh <path/to/test_anvil>
# Exit 77 = skipped (anvil/forge/cast not installed). No network, no funds.
set -eu
bin=$1
here=$(cd "$(dirname "$0")" && pwd)
contracts=$(cd "$here/../../contracts" && pwd)
for t in anvil forge cast; do
    command -v $t >/dev/null 2>&1 || { echo "anvil_test: $t not found (install foundry); skipped"; exit 77; }
done
port=${HPPLITE_ANVIL_PORT:-$((8600 + $$ % 200))}
rpc="http://127.0.0.1:$port"
# anvil's first five default accounts (public test keys, 10000 ETH each)
key0=0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
key1=0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d
key2=0x5de4111afa1a4b94908f83103eb1f1706367c2e68ca870fc3fb9a804cdab365a
key3=0x7c852118294e51e653712a81e05800f419141751be58f605c371e15141b007a6
key4=0x47e179ec197488593b187f80a00eb0da91f1b9d0b13f8733639f19c30a34926a
log=${TMPDIR:-/tmp}/hpplite-anvil-$$.log
# HPP Sepolia's block gas limit (2^50), not anvil's default 30M: a full
# 128 KB batch in contract storage needs ~90M gas (#51)
anvil --port "$port" --chain-id 31337 --silent --block-time 1 --gas-limit 1125899906842624 >"$log" 2>&1 &
pid=$!
trap 'kill -CONT $pid 2>/dev/null; kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$log"' EXIT INT TERM
for i in $(seq 1 50); do cast chain-id --rpc-url "$rpc" >/dev/null 2>&1 && break; sleep 0.2; done
cast chain-id --rpc-url "$rpc" >/dev/null 2>&1 || { echo "anvil did not start"; cat "$log"; exit 1; }
(cd "$contracts" && HPPLITE_PRIVATE_KEY=$key0 forge script script/DeployFactory.s.sol --rpc-url "$rpc" --broadcast >"$log.forge" 2>&1) \
    || { echo "factory deployment failed"; cat "$log.forge"; exit 1; }
# the console line's format changes between forge versions; the broadcast record does not
factory=$(sed -n 's/.*"contractAddress": *"\(0x[0-9a-fA-F]\{40\}\)".*/\1/p' "$contracts/broadcast/DeployFactory.s.sol/31337/run-latest.json" | head -1)
rm -f "$log.forge"
[ -n "$factory" ] || { echo "factory address not found in broadcast record"; exit 1; }
# forge script prints the simulated address even when the broadcast reverted (e.g. over the 24 KB size limit)
[ "$(cast code "$factory" --rpc-url "$rpc" 2>/dev/null | wc -c)" -gt 10 ] || { echo "factory at $factory has no code: deployment reverted (forge build --sizes)"; exit 1; }
echo "anvil on $rpc, factory $factory"
HPPLITE_ANVIL_RPC=$rpc HPPLITE_ANVIL_FACTORY=$factory HPPLITE_ANVIL_KEY0=$key0 HPPLITE_ANVIL_KEY1=$key1 HPPLITE_ANVIL_KEY2=$key2 HPPLITE_ANVIL_KEY3=$key3 HPPLITE_ANVIL_KEY4=$key4 HPPLITE_ANVIL_PID=$pid "$bin"
