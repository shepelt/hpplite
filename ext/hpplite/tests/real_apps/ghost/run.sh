#!/bin/sh
# Run the Ghost in <dir>, unmodified, with HPPLite on its database.
#   run.sh <dir> <hpplite module>   with HPPLITE_PARAMS set (rpc=...&factory=...&chainid=...&privkey=...)
# The preload puts HPPLite into the SQLite of Ghost's own better-sqlite3 before Ghost
# opens anything; HPPLITE_FILES names Ghost's database.
set -eu
dir=$(cd "$1" && pwd); ext=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")    # absolute: Ghost loads it from its own directory
b=$(cd "$dir"/versions/6.73.0/node_modules/.pnpm/better-sqlite3@*/node_modules/better-sqlite3 && pwd -P)
pre=$dir/hpplite-preload.js
printf "const D = require('%s');\nnew D(':memory:').loadExtension(process.env.HPPLITE_EXT);\n" "$b" > "$pre"
cd "$dir"
export NODE_ENV=development NODE_OPTIONS="--require $pre" HPPLITE_EXT="$ext" HPPLITE_FILES="*/ghost-local.db"
export HPPLITE_PARAMS="$HPPLITE_PARAMS&datadir=$dir/content/data&interval=1s"
exec node current/index.js
