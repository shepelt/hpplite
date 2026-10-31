#!/bin/sh
# Run by anvil_test.sh with the chain in the environment; see run_orm.sh.
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
case $HPPLITE_ORM_SUITE in
    sqlalchemy) exec "$HPPLITE_ORM_CACHE/venv/bin/python" -u test_sqlalchemy.py ;;
    django)     exec "$HPPLITE_ORM_CACHE/venv/bin/python" -u test_django.py ;;
    drizzle|knex|env_open|better_sqlite3)
        export HPPLITE_ORM_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-orm-XXXXXX")
        # better-sqlite3 opens plain paths: HPPLite is named from the environment (#55)
        export HPPLITE_FILES="*/env_open*.db:*/better_sqlite3.db"
        export HPPLITE_PARAMS="rpc=$HPPLITE_ANVIL_RPC&factory=$HPPLITE_ANVIL_FACTORY&chainid=31337&privkey=$HPPLITE_ANVIL_KEY0&datadir=$HPPLITE_ORM_DIR&interval=0"
        NODE_PATH=$HPPLITE_ORM_CACHE/node/node_modules node --no-warnings "test_$HPPLITE_ORM_SUITE.js" || exit 1
        exec "$HPPLITE_ORM_CACHE/venv/bin/python" -u orm_common.py "$HPPLITE_ORM_SUITE.db" ;;
esac
echo "unknown suite $HPPLITE_ORM_SUITE"; exit 1
