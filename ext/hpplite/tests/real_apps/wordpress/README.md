# WordPress on HPPLite (#58)

Stock WordPress with the official [SQLite Database Integration](https://wordpress.org/plugins/sqlite-database-integration/)
plugin (the WordPress Performance team's; what WordPress Playground runs on). The plugin
translates WordPress's MySQL to SQLite through PHP PDO. Nothing in WordPress or the plugin is
changed: a PHP `auto_prepend_file` loads HPPLite into PHP's SQLite (`Pdo\Sqlite::loadExtension`,
PHP 8.4), and `HPPLITE_FILES` names the database (`wp-content/database/.ht.sqlite`).

```bash
tests/real_apps/wordpress/setup.sh /tmp/wp
export HPPLITE_PARAMS="rpc=...&factory=...&chainid=...&privkey=0x..."
P="tests/real_apps/wordpress/php.sh /path/to/php8.4 build_real/hpplite.dylib /tmp/wp"
$P /tmp/wp/wp-cli.phar --path=/tmp/wp/wordpress core install --url=http://localhost:8090 ...
(cd /tmp/wp/wordpress && $P -S localhost:8090)          # web traffic, REST API
```

`session.sh <php> <module> <dir> [port]` runs the whole session below (install, WP-CLI,
pages and REST writes); then the replica check:

```bash
HPPLITE_EXT=build_real/hpplite.dylib HPPLITE_ANVIL_RPC=... HPPLITE_ANVIL_FACTORY=... \
  HPPLITE_ANVIL_KEY0=0x... HPPLITE_ANVIL_CHAINID=... HPPLITE_ORM_DIR=/tmp/wp/wordpress/wp-content/database \
  python tests/orm/orm_common.py .ht.sqlite
```

## Results (2026-10-30, anvil; PHP 8.4.23 static build, SQLite 3.45.2)

- `wp core install`: installed. One statement refused, by design (#53): the
  installer's `DELETE FROM wp_options WHERE option_name REGEXP ...`. SQLite has
  no REGEXP; the plugin implements it as a PHP function no replica has. On a
  fresh install there is nothing for it to delete. (WordPress core uses REGEXP
  in writes only there and in upgrade.php.)
- A session through WP-CLI - 5 users, 100 posts and 20 pages generated,
  categories and tags on 30 posts, 20 edits, 6 deletes, 15 comments, options,
  a theme switch, a plugin activation, due cron events, post meta - and
  through the web: front pages, 5 posts created through the REST API with an
  application password. No refusals, no HPPLite errors.
- Each WP-CLI command and each `php -S` request is its own HPPLite instance;
  every one posted its writes when it closed (135 batches).
- A replica rebuilt from the chain alone equals WordPress's database: 22
  tables, 908 rows, same state root; the checkpoint root at batch 100 was
  compared too.

## HPP Sepolia (2026-10-30)

The same session (`session.sh`) on a fresh WordPress and a fresh wallet: no
refusals but the installer's REGEXP one, no HPPLite errors, every WP-CLI
command and REST write (5 posts, a comment: 201) through. A replica rebuilt
from HPP Sepolia alone equals WordPress's database: 22 tables, 909 rows,
153 batches, same root, the checkpoint root compared. 506 transactions,
0.0081 ETH: each of the ~75 PHP processes claims the lease when it opens the
database and posts what it wrote when it closes.
