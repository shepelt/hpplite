#!/bin/sh
# Install stock Ghost with SQLite in <dir>, and rebuild its better-sqlite3 against
# a stock SQLite amalgamation (better-sqlite3's bundled SQLite truncates bound
# strings in expanded SQL, SQLITE_TRACE_SIZE_LIMIT=32, which HPPLite refuses).
#   setup.sh <dir> <amalgamation dir: sqlite3.c, sqlite3.h, sqlite3ext.h>
# Needs Node >= 24.20 and pnpm >= 11.10 on PATH (Ghost 6.73's requirements).
set -eu
dir=$1; amalg=$(cd "$2" && pwd)
mkdir -p "$dir" && cd "$dir"
npx -y ghost-cli@latest install 6.73.0 --local --no-start --no-prompt
b=$(cd versions/6.73.0/node_modules/.pnpm/better-sqlite3@*/node_modules/better-sqlite3 && pwd -P)
gyp=$(dirname "$(dirname "$(command -v npm)")")/lib/node_modules/npm/node_modules/node-gyp/bin/node-gyp.js
(cd "$b" && node "$gyp" rebuild --release --sqlite3="$amalg")
echo "better-sqlite3 at $b"
