# Rails 8 on HPPLite (#19 validation)

A Rails 8 app in production mode, as `rails new` makes it: four SQLite
databases (primary, Solid Cache, Solid Queue, Action Cable), Puma forking
workers, Solid Queue running jobs in its own processes. HPPLite is loaded
the Rails way, with `extensions:` in `config/database.yml`; the environment
does the rest: `HPPLITE_FILES` names the four files and one `HPPLITE_PARAMS`
(one key) with `dbname={file}` gives each its own rollup (#61). Nothing else
in Rails is changed. The app
is two scaffolds (`Product`, `Order`) and a job (`order_job.rb`) that a
Solid Queue worker runs for every order: it decrements the product's stock
and marks the order fulfilled, in one transaction, and writes the cache.

```bash
HPP=$PWD                                               # this tree: paths below are absolute
tests/real_apps/rails/setup.sh /tmp/rails && cd /tmp/rails/shop
export HPPLITE_EXT=$HPP/build_real/hpplite.dylib RAILS_ENV=production SECRET_KEY_BASE=... \
       HPPLITE_FILES="*/storage/production*.sqlite3" \
       HPPLITE_PARAMS="rpc=...&factory=...&chainid=...&privkey=0x...&interval=1s&dbname={file}"
export OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES        # macOS, forking servers: below
bin/rails db:prepare
WEB_CONCURRENCY=2 RAILS_MAX_THREADS=5 bin/rails server -p 3010 &
bin/jobs &
python3 $HPP/tests/real_apps/rails/drive.py http://localhost:3010 16 50
# stop both with SIGTERM (each connection posts what it captured as it closes), then per database:
HPPLITE_ANVIL_KEY0=0x... HPPLITE_ORM_EXTRA="&dbname=production_queue.sqlite3" \
  HPPLITE_ORM_DIR=/tmp/rails/shop/storage ... python $HPP/tests/orm/orm_common.py production_queue.sqlite3
```

## In a container (2026-11-03, anvil; Ruby 3.3 image, Debian trixie; page capture, the default)

`tests/real_apps/docker/rails.sh` runs the same session with Ruby, Rails and
the generated app in a container (`docker/Dockerfile.rails`, the Linux
`hpplite.so` from `docker/so.sh`), anvil and the factory on the host, and
the four replica checks on the host, each database found by its name.

- `db:prepare` on the four databases, each getting its own rollup by name;
  Puma with two workers and Solid Queue's processes writing at once; 16
  clients x 50 operations, every request through. No HPPLite message but the
  four "creating one" of the named rollups.
- Replicas rebuilt from the chain equal the writers: primary 5 tables, 380
  rows; Solid Cache 6 rows; Solid Queue 16 tables, 327 rows; Action Cable 3
  rows. Page batches: 6, 3, 8 and 1.

## Results (2026-10-31, anvil, `capture=rows`; Rails 8.1.4, Ruby 4.0.9, sqlite3 gem 2.10.2 / SQLite 3.54.1)

- `db:prepare`: every migration and schema load on the four databases, captured.
- 16 clients x 50 operations against 2 Puma workers x 5 threads, Solid Queue's
  dispatcher and worker processes running jobs at the same time: every request
  succeeded (270 orders, 149 products, 124 edits, 44 deletes, 223 reads; write
  p50 15-19 ms). All 226 orders that were not deleted were fulfilled by a job.
  The job's own log shows 43 `RecordNotFound`s: orders the driver deleted
  before their job ran - the application's race, the same without HPPLite.
- Replicas rebuilt from the chain alone equal the writers: primary (5 tables,
  381 rows), Solid Cache (6 rows), Solid Queue (16 tables, 327 rows, 20
  batches - written by the web workers and Solid Queue's processes at once).

## Found on the way

- Rails loads `extensions:` on every connection it opens. HPPLite's first load
  registers it for every later connection, so the second load found it already
  there and failed ("hpplite is already loaded in this connection"). A load
  into a connection this copy of HPPLite is already on is now a no-op; two
  different copies are still refused (`test_loadable`).
- One rollup per wallet: one key could not serve several database files
  (#61). Now a database can be named (`dbname=`, `{file}` for its file
  name): it gets its own rollup among the wallet's, and a replica finds it by
  the name in its first batch. The run again that way (one key for all four,
  plain paths in `database.yml`): every request through, 226 of 226 orders
  fulfilled, replicas rebuilt by name equal (primary 380 rows, cache 6,
  queue 327 in 16 batches).
- macOS: a Puma worker forked from the master then loading the module (it
  links libcurl, which brings in Objective-C frameworks) is killed by macOS's
  fork-safety check. `OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES`, the usual
  setting for forking Ruby servers on macOS. Linux has no such check.
- A process that exits without closing its connections (a rake task) leaves
  its captures pending in the file; they are posted when the database is next
  opened. The Action Cable database, which nothing opens at run time, kept the
  8 rows of its schema load pending until then.
