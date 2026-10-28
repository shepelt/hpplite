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
# anvil's first two default accounts (public test keys, 10000 ETH each)
key0=0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
key1=0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d
log=${TMPDIR:-/tmp}/hpplite-anvil-$$.log
anvil --port "$port" --chain-id 31337 --silent --block-time 1 >"$log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$log"' EXIT INT TERM
for i in $(seq 1 50); do cast chain-id --rpc-url "$rpc" >/dev/null 2>&1 && break; sleep 0.2; done
cast chain-id --rpc-url "$rpc" >/dev/null 2>&1 || { echo "anvil did not start"; cat "$log"; exit 1; }
factory=$(cd "$contracts" && HPPLITE_PRIVATE_KEY=$key0 forge script script/DeployFactory.s.sol --rpc-url "$rpc" --broadcast 2>&1 | sed -n 's/.*HPPLiteFactory deployed at: *//p' | head -1)
[ -n "$factory" ] || { echo "factory deployment failed"; exit 1; }
echo "anvil on $rpc, factory $factory"
HPPLITE_ANVIL_RPC=$rpc HPPLITE_ANVIL_FACTORY=$factory HPPLITE_ANVIL_KEY0=$key0 HPPLITE_ANVIL_KEY1=$key1 "$bin"
