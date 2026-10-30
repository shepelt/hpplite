#!/bin/sh
# Stock WordPress + the official SQLite Database Integration plugin in <dir>.
#   setup.sh <dir>        then run WP-CLI / php -S through php.sh
set -eu
dir=$1; mkdir -p "$dir" && cd "$dir"
curl -sL -o wp.zip https://wordpress.org/latest.zip && unzip -qo wp.zip
curl -sL -o sqlite.zip https://downloads.wordpress.org/plugin/sqlite-database-integration.zip
unzip -qo sqlite.zip -d wordpress/wp-content/plugins/
curl -sL -o wp-cli.phar https://raw.githubusercontent.com/wp-cli/builds/gh-pages/phar/wp-cli.phar
P=$(pwd)/wordpress/wp-content/plugins/sqlite-database-integration
sed -e "s#{SQLITE_IMPLEMENTATION_FOLDER_PATH}#$P#" -e "s#{SQLITE_PLUGIN}#sqlite-database-integration/load.php#g" \
    "$P/db.copy" > wordpress/wp-content/db.php
mkdir -p wordpress/wp-content/database
cp wordpress/wp-config-sample.php wordpress/wp-config.php
python3 - wordpress/wp-config.php <<'PY'
import sys
p = sys.argv[1]; s = open(p).read()
s = s.replace('/* Add any custom values between this line and the "stop editing" line. */',
  '/* Add any custom values between this line and the "stop editing" line. */\n'
  "define( 'DB_DIR', __DIR__ . '/wp-content/database/' );\ndefine( 'DB_FILE', '.ht.sqlite' );\n"
  "define( 'WP_ENVIRONMENT_TYPE', 'local' );")
open(p, 'w').write(s)
PY
