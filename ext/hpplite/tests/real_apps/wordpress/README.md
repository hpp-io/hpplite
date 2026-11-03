# WordPress on HPPLite (#58)

Stock WordPress with the official [SQLite Database Integration](https://wordpress.org/plugins/sqlite-database-integration/)
plugin (the WordPress Performance team's; what WordPress Playground runs on). The plugin
translates WordPress's MySQL to SQLite through PHP PDO. Nothing in WordPress or the plugin is
changed: a PHP `auto_prepend_file` loads HPPLite into PHP's SQLite (`Pdo\Sqlite::loadExtension`,
PHP 8.4), and `HPPLITE_FILES` names the database (`wp-content/database/.ht.sqlite`).

```bash
tests/real_apps/wordpress/setup.sh /tmp/wp
export HPPLITE_PARAMS="rpc=...&factory=...&chainid=...&privkey=0x..."
P="$PWD/tests/real_apps/wordpress/php.sh /path/to/php8.4 $PWD/build_real/hpplite.dylib /tmp/wp"   # absolute: run from other directories below
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

## In a container (2026-11-03, anvil; PHP 8.4 image, Debian trixie; page capture, the default)

`tests/real_apps/docker/wordpress.sh` runs the same session with PHP and
WordPress in a container (`docker/Dockerfile.wordpress`, the Linux
`hpplite.so` built on the image's Debian by `docker/so.sh`), anvil and the
factory on the host, and the replica check on the host. Nothing on the host
but Docker, foundry and `build_real/hpplite.dylib`.

- The session: every WP-CLI command, the pages and the REST writes, no
  failure, no HPPLite message. 131 batches of pages on the chain.
- The replica rebuilt from the chain equals the site: 22 tables, 871 rows,
  the same root at height 132.

## Results (2026-10-30, anvil; PHP 8.4.23 static build, SQLite 3.45.2; row capture)

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

## Concurrent: several PHP workers at once (2026-10-30, anvil)

What a production setup does: many PHP worker processes, each request its own
database connection, all writing at once. `PHP_CLI_SERVER_WORKERS=8 php -S`
(the built-in server forking 8 workers - php-fpm's process model) and
`hpplite-sequencer` holding the database, so every request's connection is a
follower that captures and closes without waiting for the chain:

```bash
HPPLITE_PARAMS="$HPPLITE_PARAMS&datadir=/tmp/wp/wordpress/wp-content/database&interval=1s" \
  build_real/hpplite-sequencer /tmp/wp/wordpress/wp-content/database/.ht.sqlite &
(cd /tmp/wp/wordpress && PHP_CLI_SERVER_WORKERS=8 $P -S localhost:8093) &
python3 tests/real_apps/wordpress/load.py http://localhost:8093 admin <application password> 16 50
```

`load.py`: 16 clients at once, 50 operations each - new posts, edits,
comments through the REST API, API and page reads.

- Every write succeeded (245 posts, 109 edits, 152 comments); no refusals
  but the installer's REGEXP one.
- A replica rebuilt from the chain equals WordPress's database: 22 tables,
  1714 rows, 27 batches. 39 transactions in all (one lease claim, the
  batches) - the single-process runs above took ~3 per batch, every PHP
  process claiming and releasing the lease.
- Same load without HPPLite: 21.4 s and a write p50 of ~430 ms; with it,
  22.2 s and ~440 ms (~4%).

The same on HPP Sepolia (fresh site, fresh wallet): every write succeeded
(16 clients x 50, 20.1 s), the replica rebuilt from HPP Sepolia equals
WordPress's database (22 tables, 1714 rows, 70 batches), 82 transactions,
0.0267 ETH. The cost is WordPress's own write volume: 2.18 MB of SQL in 3142
statements, ~4.3 KB per web write - 46% `INSERT INTO wp_posts` (each post and
the revision WordPress keeps on every edit, all 23 columns spelled out by the
plugin), 18% post updates, 11% comments, 5% the plugin's schema bookkeeping.

## Row capture (`capture=rows`, #19; 2026-10-31, anvil)

The concurrent run again on a fresh site with `capture=rows` in
`HPPLITE_PARAMS` (8 workers, the sequencer, 16 clients x 50):

- No refusals at all: the installer's `DELETE ... REGEXP` - the one statement
  statement capture refuses in WordPress - is recorded as the rows it
  deleted, and every write succeeded (245 posts, 109 edits, 152 comments).
- 21.8 s and a write p50 of ~450 ms (statement capture: 22.2 s, ~440 ms).
- A replica rebuilt from the chain equals WordPress's database: 22 tables,
  1718 rows, same state root.
- 626 KB of batches for the session, against 2.18 MB of SQL with statement
  capture: the plugin spells out every column name of every `wp_posts`
  INSERT; a record carries the values.

Found on the way, fixed: a second process on the file was not a follower
(each sequenced on its own); no-op statements from other connections could
vanish from under a batch being built; and closing a connection waited out
HPPLite's 100 ms timer sleep - most of what it added to each PHP request.
