#!/bin/sh
# Print a fresh private key funded on the local chain (one rollup per wallet: a fresh wallet is a fresh database)
set -eu
rpc=${1:-http://127.0.0.1:8745}
out=$(cast wallet new 2>/dev/null)
key=$(echo "$out" | sed -n 's/.*Private key: *\(0x[0-9a-fA-F]*\).*/\1/p')
addr=$(echo "$out" | sed -n 's/.*Address: *\(0x[0-9a-fA-F]*\).*/\1/p')
cast send --rpc-url "$rpc" --private-key 0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80 \
    --value 100ether "$addr" >/dev/null 2>&1
echo "$key"
