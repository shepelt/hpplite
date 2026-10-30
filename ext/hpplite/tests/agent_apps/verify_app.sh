#!/bin/sh
# Rerun an agent-built app from scratch and check its database against the chain.
#   tests/agent_apps/verify_app.sh <app dir> <rpc> <factory> <chainid> <port> [anvil|sepolia]
# A fresh funded wallet (= a fresh rollup) and a fresh data directory; the app's own
# scenario.sh drives it over HTTP; then a replica rebuilt from the chain alone must equal
# the app's database: same height, same state root, same schema, same rows in every table
# (sqlite_sequence included). Needs the ORM venv (tests/orm/run_orm.sh creates it).
set -u
app=$(cd "$1" && pwd); rpc=$2; factory=$3; chainid=$4; port=$5; net=${6:-anvil}
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
cache=${HPPLITE_ORM_CACHE:-${TMPDIR:-/tmp}/hpplite-orm}
run=${HPPLITE_AGENT_RUNS:-${TMPDIR:-/tmp}/hpplite-agent-runs}/$(basename "$app")-$(date +%s); mkdir -p "$run"
key=$("$here/newkey_$net.sh" "$rpc")
export APP_PORT=$port APP_DB=$run/app.db HPPLITE_EXT=${HPPLITE_EXT:-$root/build_real/hpplite.dylib}
export HPPLITE_URI_PARAMS="rpc=$rpc&factory=$factory&chainid=$chainid&privkey=$key&datadir=$run&interval=1s"
echo "== $(basename "$app"): scenario (data in $run)"
t0=$(date +%s)
(cd "$app" && sh ./scenario.sh) > "$run/scenario.log" 2>&1
rc=$?
echo "   scenario exit $rc in $(( $(date +%s) - t0 ))s; $(grep -c 'statement refused' "$run/scenario.log") refusals"
[ $rc -eq 0 ] || { tail -15 "$run/scenario.log"; exit 1; }
HPPLITE_ANVIL_RPC=$rpc HPPLITE_ANVIL_FACTORY=$factory HPPLITE_ANVIL_KEY0=$key HPPLITE_ANVIL_CHAINID=$chainid \
    HPPLITE_ORM_DIR=$run "$cache/venv/bin/python" -u "$root/tests/orm/orm_common.py" app.db 2>&1 \
    | tee "$run/verify.log" | sed 's/^/   /'
grep -q "0 failure" "$run/verify.log"
