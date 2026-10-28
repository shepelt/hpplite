#!/bin/sh
# Serve the HPPLite timeline demo and expose it with a Cloudflare quick tunnel.
#   ext/hpplite/examples/demo/run_demo.sh            # needs ext/hpplite/.env (HPPLITE_PRIVATE_KEY)
# Only the private key is taken from .env: the old HPPLITE_CONTRACT_ADDRESS there
# must not be used (the rollup comes from the factory via l1=hpp-sepolia).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
ext=${HPPLITE_EXT:-$root/build_real/hpplite.dylib}
py=${PYTHON:-/opt/homebrew/bin/python3.13}
port=${PORT:-8787}
[ -f "$ext" ] || { echo "build the real-L1 tree first: cmake -S $root -B $root/build_real -DHPPLITE_USE_REAL_L1=ON -DHPPLITE_SQLITE_SRC=<amalgamation dir> && cmake --build $root/build_real"; exit 1; }
if [ -z "${HPPLITE_PRIVATE_KEY:-}" ]; then
    HPPLITE_PRIVATE_KEY=$(sed -n 's/^HPPLITE_PRIVATE_KEY=//p' "$root/.env" | head -1)
    export HPPLITE_PRIVATE_KEY
fi
[ -n "$HPPLITE_PRIVATE_KEY" ] || { echo "HPPLITE_PRIVATE_KEY not set"; exit 1; }
unset HPPLITE_CONTRACT_ADDRESS HPPLITE_FACTORY_ADDRESS HPPLITE_L1_RPC HPPLITE_DATA_DIR
cd "$here"
"$py" app.py "$ext" "$port" > "$here/data/app.log" 2>&1 &
app=$!
trap 'kill $app 2>/dev/null; wait $app 2>/dev/null' EXIT INT TERM
sleep 2
kill -0 $app 2>/dev/null || { cat "$here/data/app.log"; exit 1; }
echo "app: http://localhost:$port (log: data/app.log)"
# the quick-tunnel API is occasionally slow to answer; keep asking
while kill -0 $app 2>/dev/null; do
    cloudflared tunnel --url "http://localhost:$port" --edge-ip-version 4 --protocol http2 2>&1 | grep --line-buffered -E "trycloudflare.com|error|failed"
    sleep 5
done
