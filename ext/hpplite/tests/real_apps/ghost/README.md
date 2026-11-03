# Ghost on HPPLite (#58)

Stock [Ghost](https://ghost.org) 6.73.0 with its SQLite database under HPPLite.
Ghost's code is untouched. Two changes are made around it:

- Ghost's better-sqlite3 is rebuilt against a stock SQLite amalgamation
  (`setup.sh`). Its bundled SQLite is compiled with
  `SQLITE_TRACE_SIZE_LIMIT=32`, which cuts bound strings in expanded SQL;
  HPPLite refused it at open when this run was made (statement capture).
  With row capture (the default now) the bundled SQLite serves as it is
  (#62, `tests/orm/test_better_sqlite3.js`), and the rebuild is no longer
  needed; this setup keeps it so the run stays reproducible.
- A `NODE_OPTIONS=--require` preload loads HPPLite into that SQLite, and
  `HPPLITE_FILES` names Ghost's database (`run.sh`).

```bash
tests/real_apps/ghost/setup.sh /tmp/ghost build          # Node >= 24.20, pnpm >= 11.10
HPPLITE_PARAMS="rpc=...&factory=...&chainid=...&privkey=0x..." \
  tests/real_apps/ghost/run.sh /tmp/ghost build_real/hpplite.dylib &
python3 tests/real_apps/ghost/drive.py                     # the Admin API session
# stop Ghost (SIGINT), then the replica check:
HPPLITE_ANVIL_RPC=... HPPLITE_ANVIL_FACTORY=... HPPLITE_ANVIL_KEY0=0x... \
  HPPLITE_ORM_DIR=/tmp/ghost/content/data python tests/orm/orm_common.py ghost-local.db
```

`drive.py` runs a publisher's first day through the Admin API: owner setup,
staff session, 10 tags, 60 posts (Lexical; drafts, published, scheduled,
featured), 20 edits, 5 deletes, 8 pages, 150 members with labels, a
settings change and a newsletter.

Ghost does not close its database when it stops (SIGINT), so HPPLite's
final flush on close never runs: before stopping it, wait until the file's
`_hpplite_pending` is empty (read it with plain sqlite3, read-only). What is
left is not lost - it stays in the file and the next open posts it - but the
lease the dead instance held must expire first (minutes).

## In a container (2026-11-03, anvil; Node 24 and pnpm 11 image, Debian trixie; page capture, the default)

`tests/real_apps/docker/ghost.sh`: Ghost installed by ghost-cli in the image
(`docker/Dockerfile.ghost`), better-sqlite3 rebuilt at run time against the
build volume's amalgamation, the Linux `hpplite.so` from `docker/so.sh`,
anvil and the factory on the host, the Admin API session inside, the replica
check on the host.

- Ghost up 4 s after its migrations and fixtures; the session: 60 posts, 150
  members, every call succeeded, no HPPLite message in Ghost's log. 11 page
  batches.
- The replica rebuilt from the chain equals Ghost's database: 102 tables,
  2208 rows, the same root at height 12.

## Results (2026-10-30, anvil)

- First boot, all of Ghost's migrations and fixtures (106 tables): clean,
  three times in a row.
- Admin API session: every call succeeded.
- A replica rebuilt from the chain alone equals Ghost's database: 102 tables,
  2208 rows, 15 batches, same state root.
- Found on the way, fixed in 280a364cf2: a finalize deferred while Ghost had
  a statement open was taken for a fork; a batch built while a no-op
  statement was being unlogged carried a row the log no longer had.

## Results (2026-10-30, HPP Sepolia)

- First boot (migrations and fixtures posted as Ghost starts): 45 s.
- Admin API session: every call succeeded.
- A replica rebuilt from HPP Sepolia alone equals Ghost's database: 102
  tables, 2207 rows, 35 batches, same state root. ~0.006 ETH of gas.
- Found on the way, fixed in the next commit: the first attempt reused the
  database from the anvil run with a fresh wallet; HPPLite took "this
  database is ahead of L1" for a reorg and re-posted the anvil history's
  first batch into the new rollup. A database is now bound to its rollup.

## Row capture (`capture=rows`, #19; 2026-10-31, anvil)

- First boot, all migrations and fixtures with records instead of
  statements: booted in 16 s, no refusals, no HPPLite errors.
- Admin API session (setup, 60 posts, 150 members, 34 published): every
  call succeeded.
- A replica rebuilt from the chain alone equals Ghost's database: 102 tables,
  2207 rows, 17 batches, same state root.
