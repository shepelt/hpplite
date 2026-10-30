#!/bin/sh
# Return what is left in the wallets newkey_sepolia.sh created to the test wallet.
set -u
here=$(cd "$(dirname "$0")" && pwd)
rpc=${1:-https://sepolia.hpp.io}
master=$(cast wallet address "$(sed -n 's/^HPPLITE_PRIVATE_KEY=//p' "$here/../../.env" | head -1)")
[ -f "$here/.keys" ] || exit 0
gp=$(cast gas-price --rpc-url "$rpc"); gp=$((gp * 2))
while read -r k; do
    a=$(cast wallet address "$k"); b=$(cast balance "$a" --rpc-url "$rpc")
    v=$((b - 21000 * gp))
    [ "$v" -gt 0 ] && cast send --rpc-url "$rpc" --private-key "$k" --gas-price "$gp" --gas-limit 21000 --value "$v" "$master" >/dev/null 2>&1 \
        && echo "swept $(cast from-wei $v) from $a"
done < "$here/.keys"
