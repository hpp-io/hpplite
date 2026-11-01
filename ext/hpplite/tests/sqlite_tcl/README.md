# SQLite's own tests with HPPLite on every connection (#62)

SQLite's TCL suite (`test/*.test` in this tree), run twice: on a stock
`testfixture`, and with HPPLite loaded into it and turned on for every file
a test opens (`HPPLITE_FILES="*"`, row capture, no chain). What fails only
with HPPLite is what HPPLite changes about SQLite's behaviour.

```bash
tests/sqlite_tcl/run.sh /tmp/sqlite-tcl build_mock/hpplite.dylib insert update trigger1 fkey2 ...
```

`run.sh` builds a stock `testfixture` from this tree the first time (Tcl
with `tclConfig.sh`: `TCL_CONFIG_DIR`), then prints per file the summary
line of each run and the tests that fail only with HPPLite (those that
compared unequal and those that threw). `wrap.tcl` loads the HPPLite module
into the test process - `tester.tcl` clears automatic extensions as it
starts, so the wrapper loads HPPLite again right after - and keeps the
harness out of HPPLite's way where the harness, not the test, would stop
the file:

- the helpers that drop or empty every table (`drop_all_tables`,
  `delete_all_data`, `altercol`'s `drop_all_tables_and_views`, and the two
  loops `alter.test` and `insert3.test` carry themselves) leave HPPLite's
  tables out, which HPPLite refuses to write; the tests' own statements are
  untouched;
- the delete helpers (`forcedelete`, `delete_file`, so `reset_db`), told a
  database's main file, delete its `-wal`, `-shm`, `-hpplite` and
  `-hpplite-users` with it: HPPLite keeps files in WAL, and a `-shm` left
  behind (read-only, after `delete.test`'s read-only section) made the next
  database on that name read-only;
- a file HPPLite cannot start on opens without it, as stock SQLite opens it,
  and the log says so (`hpplite-wrap:` lines): the file's tests run on, on
  stock SQLite for that connection. Three things cause it: a file format
  SQLite cannot read (`alter2-4.1`, stock opens lazily and fails at the
  first query) and, before #124 and #123, a schema rewritten whole through
  `writable_schema` (below) and a second connection opened while the first
  holds a write transaction (`savepoint-5.4.2`: open now reads when the
  bookkeeping is current).

A few tests shut SQLite down mid-file; HPPLite is off from there on in those.

## Results (2026-11-01; 96 files, the parts row capture and the authorizer touch)

Of the 66 files of 2026-10-31, 34 run with the same result as stock SQLite
(the 31 of then, and `delete`, `insert3`, `trigger7` now that the harness
deletes a database whole and opens what HPPLite refuses), 32 differ, none
stops part way. Everything that differed on 2026-10-31 differs the same way
after today's refactoring (`hpplite.c` split, `capture.c`, the deferred
rebuild for stale schemas, #108/#115/#116): the only lines that moved are
the fault-injection iterations in `triggerA` and `vacuum3`, where the
allocation count at which an injected failure hits HPPLite's own checks
shifted (the same number of tests fails).

30 more files (`e_insert`, `e_update`, `e_delete`, `e_createtable`,
`e_dropview`, `e_droptrigger`, `e_reindex`, `triggerE`-`G`, `savepoint6`,
`savepoint7`, `rowid`, `null`, `default`, `check`, `notnull`, `notnull2`,
`unique`, `unique2`, `index`, `index2`, `index3`, `view`, `view2`, `with1`,
`with2`, `with3`, `upsertfault`, `without_rowid7`): 18 as stock, 8 differ,
4 stop part way (`e_update`, `e_createtable`, `e_dropview`,
`e_droptrigger`: an ATTACH in their setup, refused, and nothing after it
runs).

Together, 52 of 96 as stock: `conflict`-`conflict3`, `delete`-`delete4`,
`e_insert`, `fkey3`, `fkey4`, `fkey6`, `index2`, `index3`, `insert3`,
`lastinsert`, `notnull`, `notnull2`, `null`, `returning1`, `rowid`,
`savepoint2`, `savepoint6`, `savepoint7`, `trigger2`-`trigger9`,
`triggerB`, `triggerE`-`triggerG`, `unique`, `unique2`, `update2`,
`upsert1`-`upsert5`, `upsertfault` (more fault iterations, none failing),
`vacuum4`, `view2`, `with3`, `without_rowid2`, `without_rowid4`-`7`.
A full run of the 96 takes about 7.5 minutes on an M-series Mac.

What fails in the other 44, by cause:

- **HPPLite's own tables and triggers** (most of them): tests that list the
  schema (`SELECT name FROM sqlite_master`, `PRAGMA table_list`), count its
  rows, check a root page or a page count, trace the statements a write
  runs, or print a query plan see `_hpplite_*` tables and the
  `hpplite_cap_*` triggers; `total_changes` counts the rows the capture
  triggers write (`insert2-3.2`: 18 for 9 inserts). `alter-1`, `alter-21`,
  `alter3`, `altercol-9`/`-10`, `alterdropcol-7.2`, `alterdropcol2`,
  `autoinc`, `check-3`, `fkey1-5.2.1`, `fkey7`, `index`, `insert-5`,
  `insert2`, `savepoint-10`, `savepoint5`, `trigger1`, `triggerC-12`,
  `update-21.12`, `vacuum`, `vacuum2`, `view-29.1`, `without_rowid1-1.50`.
- **SQLite's transfer optimization is skipped**: `INSERT INTO t SELECT *
  FROM s` copies pages directly only when `t` has no triggers, and the
  capture triggers count. The result is the same, but a whole-table copy
  runs row by row (`insert4`, `insert5-2`, `with2-5.2`).
- **Refused by design**: ATTACH (`alter-3.2`, `alter4`, `altercol-18`,
  `e_delete`, `e_reindex`, `fkey2`'s `aux` sections, `fkey5-12`, `fkey8`,
  `triggerD`, `view-13.1`, `with1-24.2`, `without_rowid3`, and the four
  files above that attach in their setup); leaving WAL (`changes.test` sets
  `journal_mode = off`); an application-defined function in a schema change
  (`check-7`: `CREATE TABLE t6(a CHECK(myfunc(a)))`, logged as text, has no
  `myfunc` on a replica); dropping or writing HPPLite's tables.
- **A cascade of a refusal**: `fkey-2.14.3.3` (a DROP TABLE that must fail
  a foreign key check succeeded) was not HPPLite's doing. `fkey2-14.1aux.6`
  runs `PRAGMA foreign_keys = off; ALTER TABLE t2 ...; PRAGMA foreign_keys
  = on` in one `execsql`; the ATTACH before it was refused, so `t2` does
  not exist, the ALTER fails, and the trailing `PRAGMA foreign_keys = on`
  never runs. Stock SQLite with foreign keys left off fails `fkey-2.14.3.3`
  the same way (bisected: of the six sub-sections of `fkey2-14`, only
  `14.1aux` before `2.14.3` makes it fail, and `14.1aux.6` alone does).
- **The schema rewritten whole**: `UPDATE sqlite_master SET sql = '...'`
  with no WHERE under `writable_schema` (`alterdropcol-8.0`, `default-4.0`;
  `trigger7-99.1` writes `'nonsense'` on purpose). Stock has one schema
  row then; with HPPLite the rows of `_hpplite_*` would be rewritten too
  and the file read as `malformed database schema (_hpplite_meta)`, so
  since #124 the statement is refused (`SQLITE_INTERRUPT`) and those tests
  fail on the refusal instead; the qualified form (`WHERE name = ...`) runs.
- **HPPLite keeps the database in WAL**: a write straight into the main
  file (`alter2`'s `hexio_write` of the schema cookie and file format;
  `get_file_format` after VACUUM) is behind the WAL's copy of page 1 and
  changes nothing the connection sees; a rollback-mode locking expectation
  (`savepoint-5.4.3` expects `database is locked` for a RELEASE while a
  reader is open) does not hold; a database deleted by its main file alone
  meets its old `-wal`/`-shm` (the harness now deletes them with it).
- **Injected failures**: an out-of-memory inside HPPLite's own checks
  refuses the statement, the safe outcome (`triggerA-3`); injected I/O
  errors (`vacuum3`: `tester.tcl` stops the file at its 1000th error, its
  `--maxerror` default).
- **HPPLite's schema reads warm SQLite's caches**: `view-32.2`/`32.3` set
  `SQLITE_LIMIT_EXPR_DEPTH` below the nesting of seven views and expect
  `nested too deep`; HPPLite's `pragma_table_list` at each schema change
  resolves the views' column names first, and the query then fits. Stock
  with a `SELECT name FROM pragma_table_list` before the limit does the same.
- **Findings** (HPPLite's behaviour, not the harness's; see below):
  `changes2` (`sqlite3_changes()` is 0 inside a write and at a RETURNING
  statement's first row); `savepoint4` (a crash simulation, not analysed).

### Findings of this run

- `changes2-1.1`, `-1.3`, `-2.4`: `sqlite3_changes()` read by a write
  statement, or at the first row of an `UPDATE ... RETURNING`, is 0. As a
  write starts, `hpplite_rows_covered_for_write` (capture_rows.c) runs
  `UPDATE main._hpplite_meta SET id = id WHERE 0` to take the write lock
  before it reads the schema version (#92, #108); a zero-row UPDATE sets
  the connection's change count to 0. `SELECT changes()` after a write is
  right (no lock is taken for a read), `sqlite3_changes()` after a write
  is right (the write's own count is set at its end); `INSERT INTO log
  VALUES(changes())` after an INSERT of two rows writes `0` (stock: `2`),
  and `sqlite3_step` of `UPDATE t SET b='v2' WHERE a=1 RETURNING a` then
  `sqlite3_changes()` gives 0 at `SQLITE_ROW` (stock: 1), 1 at
  `SQLITE_DONE`. `unlogLast` in capture.c already knows the problem from
  the other side ("a zero-row UPDATE says 0 again").
- `savepoint4` (1000 errors, as on 2026-10-31): `crashsql` runs the
  statements in a child `testfixture` with the crash VFS and a crash
  scheduled on `test.db-journal`; HPPLite's file is in WAL and the child has
  no HPPLite. From the first iteration the file reads as `database disk
  image is malformed`, in the child and in the parent's integrity check.
  Not analysed further here; it needs a look at what the child writes.

## How to read a difference

For a test that fails only with HPPLite, read the HPPLite log
(`<work dir>/out/hp/<file>/log`) above the failing test: a `hpplite:` line
names the statement HPPLite refused and why, a `hpplite-wrap:` line says a
connection opened without HPPLite, and the tests after either are its
consequences (tables that do not exist, `foreign_keys` left off, a
connection that is stock SQLite from there on). Then match the test's
expectation to the causes above: a listing or count that includes
`_hpplite_*`, a `-wal`/`-shm` or a header byte a test expects to handle
itself, an injected failure. What matches none of them is a finding: give
the test name, the statements, stock's and HPPLite's results, and a
reproduction outside the suite if there is one (a `tester.tcl` script run
through `wrap.tcl` is the quickest).

## Found and fixed by this run

- `PRAGMA count_changes=1` made every write refused (HPPLite's own writes
  returned a row), a v0.1 bug too: `conflict`, `conflict2` and `upsert1` went
  from failing to clean (ea94b1df99).
- Leaving WAL failed as "database is locked"; it is refused with the reason
  (ea94b1df99).
- A read-only connection failed to open; HPPLite stands aside for it
  (ea94b1df99).
- `fkey-2.14.3.3` turned out to be a cascade of the refused ATTACH (above),
  not a foreign key check HPPLite skipped (2026-11-01).
