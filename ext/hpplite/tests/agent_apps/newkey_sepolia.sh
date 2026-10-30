#!/bin/sh
# A fresh wallet on HPP Sepolia, funded from the project's test wallet (ext/hpplite/.env).
#   newkey_sepolia.sh [rpc] [amount]     prints the key; every key is also appended to
# tests/agent_apps/.keys (git-ignored) so sweep_sepolia.sh can return what is left.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
rpc=${1:-https://sepolia.hpp.io}; amount=${2:-${HPPLITE_STIPEND:-0.003ether}}
master=$(sed -n 's/^HPPLITE_PRIVATE_KEY=//p' "$here/../../.env" | head -1)
out=$(cast wallet new 2>/dev/null)
key=$(echo "$out" | sed -n 's/.*Private key: *\(0x[0-9a-fA-F]*\).*/\1/p')
addr=$(echo "$out" | sed -n 's/.*Address: *\(0x[0-9a-fA-F]*\).*/\1/p')
echo "$key" >> "$here/.keys"
cast send --rpc-url "$rpc" --private-key "$master" --value "$amount" "$addr" >/dev/null 2>&1
echo "$key"
