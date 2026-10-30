#!/bin/sh
# Run by anvil_test.sh with the chain in the environment; see run_orm.sh.
here=$(cd "$(dirname "$0")" && pwd)
cd "$here"
case $HPPLITE_ORM_SUITE in
    sqlalchemy) exec "$HPPLITE_ORM_CACHE/venv/bin/python" -u test_sqlalchemy.py ;;
    django)     exec "$HPPLITE_ORM_CACHE/venv/bin/python" -u test_django.py ;;
    drizzle|knex)
        export HPPLITE_ORM_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-orm-XXXXXX")
        NODE_PATH=$HPPLITE_ORM_CACHE/node/node_modules node --no-warnings "test_$HPPLITE_ORM_SUITE.js" || exit 1
        exec "$HPPLITE_ORM_CACHE/venv/bin/python" -u orm_common.py "$HPPLITE_ORM_SUITE.db" ;;
esac
echo "unknown suite $HPPLITE_ORM_SUITE"; exit 1
