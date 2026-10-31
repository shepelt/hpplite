#!/bin/sh
# PHP with HPPLite: preload via auto_prepend_file, the database named by HPPLITE_FILES.
#   php.sh <php binary> <hpplite module> <WordPress dir> args...   with HPPLITE_PARAMS set
# (auto_prepend_file does not apply to `php -r`: query from a script file)
set -eu
php=$1; dir=$(cd "$3" && pwd); export HPPLITE_EXT=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); shift 3   # absolute: PHP may chdir
here=$(cd "$(dirname "$0")" && pwd)
export HPPLITE_FILES="*/.ht.sqlite"
export HPPLITE_PARAMS="$HPPLITE_PARAMS&datadir=$dir/wordpress/wp-content/database&interval=1s"
exec "$php" -d auto_prepend_file="$here/preload.php" -d memory_limit=512M "$@"
