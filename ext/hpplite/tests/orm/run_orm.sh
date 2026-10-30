#!/bin/sh
# ORM smoke tests: each suite drives one ORM against real contracts on a
# fresh anvil chain (tests/l1/anvil_test.sh), then a replica rebuilt from
# the chain must match the writer row for row and root for root.
#   tests/orm/run_orm.sh <path/to/hpplite.(dylib|so)> [suite ...]
# Suites: sqlalchemy django drizzle knex. Needs network for the first pip/npm install
# (cached in $HPPLITE_ORM_CACHE, default $TMPDIR/hpplite-orm); not in CI.
set -eu
ext=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
here=$(cd "$(dirname "$0")" && pwd)
cache=${HPPLITE_ORM_CACHE:-${TMPDIR:-/tmp}/hpplite-orm}
suites=${*:-sqlalchemy django drizzle knex}
mkdir -p "$cache"
case " $suites " in *" "*)    # the Node suites use the venv for the replica check
    [ -x "$cache/venv/bin/python" ] || python3 -m venv "$cache/venv"
    "$cache/venv/bin/python" -c 'import sqlalchemy, alembic, django' 2>/dev/null \
        || "$cache/venv/bin/pip" -q install sqlalchemy alembic django ;;
esac
case " $suites " in *" drizzle "*|*" knex "*)
    [ -d "$cache/node/node_modules/knex" ] || (mkdir -p "$cache/node" && cd "$cache/node" \
        && { [ -f package.json ] || npm init -y >/dev/null; } && npm install -s drizzle-orm knex sqlite3) ;;
esac
fail=0
for s in $suites; do
    HPPLITE_EXT=$ext HPPLITE_ORM_SUITE=$s HPPLITE_ORM_CACHE=$cache \
        sh "$here/../l1/anvil_test.sh" "$here/suite.sh" || fail=1
done
exit $fail
