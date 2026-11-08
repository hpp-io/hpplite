# SQLite behaviours HPPLite depends on

**Status: inventory (#105). What the capture path relies on that is not in
SQLite's documented API, where it is in SQLite's source, where HPPLite uses
it, and which test fails if it changes.**

HPPLite is a plain extension (#2): it uses only public SQLite interfaces,
but it is correct only because of how some of them behave in cases the
documentation does not promise. Each entry below was found by a bug. The
`sqlite-trunk` CI job (`.github/workflows/ci.yml`) builds the extension
against the upstream `master` of github.com/sqlite/sqlite and runs the mock
suite, so a change upstream shows as a red job and not as a user's diverged
rollup. That job is advisory (`continue-on-error: true`): trunk moves every
day and must not block a release of this branch.

Source locations are in the `src/` files of this repository (the amalgamation
`build/sqlite3.c` is made from them; function names are the same there). The
branch sits on `version-3.54.1` (`patches/UPSTREAM_BASE`). "Pinned by" names
the ctest suite and the test case's `TEST("...")` line.

The list was thirteen entries long under row and statement capture
(v0.2-v0.7): the statement trace, the reprepare of a stale statement, the
rollback hook, `sqlite3_expanded_sql()`, the pre-update hook and the rest
of what a capture that reads SQL out of a running connection must know.
Page capture (#146) reads commits from the WAL file instead, and v0.8 made
it the one capture (#253), so those entries went with the code that relied
on them (git history has them, with the SQLite source locations). What is
left is what reading the WAL from a connection's hook relies on.

## Summary

| # | Behaviour | Pinned by | Trunk 2026-10-31 |
|---|---|---|---|
| 1 | The wal hook runs only when `sqlite3_step()` returns SQLITE_DONE in autocommit; a statement reset after a row commits without it | nothing since #253 (see the entry) | handled (#155) |
| 2 | A read transaction pinned at a WAL frame bounds every checkpoint there; the frames past it can be cut off the WAL file | test_wal_pin | ok (2026-11-02 trunk) |
| 3 | `SQLITE_FCNTL_DATA_VERSION` moves per own commit and per detected change, not per foreign commit | nothing since #253 | ok |

## 1. The wal hook runs only when `sqlite3_step()` returns SQLITE_DONE in autocommit

**Relied on, and worked around.** Page capture (#146) reads a commit's
frames from the WAL file when the connection's wal hook says a commit
happened (#155). SQLite runs that hook from `sqlite3_step()` when the step
returns SQLITE_DONE in autocommit, and from COMMIT. A statement stepped to
a row and then reset - `INSERT ... RETURNING` taken with one step, a
driver's `.get()` - commits inside `sqlite3_reset()`, and the hook does not
run. The frames are in the WAL all the same.

**SQLite source.** `vdbeapi.c` `sqlite3Step()`: `if( rc==SQLITE_DONE &&
db->autoCommit ){ p->rc = doWalCallbacks(db); }`; `sqlite3VdbeReset()`
halts and commits through `sqlite3VdbeHalt()` without calling
`doWalCallbacks()`.

**HPPLite code.** `hpplite.c` `pagesWalHook()` sets `bWalHookFired`;
`pagesTrace()` clears it at SQLITE_TRACE_STMT and, at SQLITE_TRACE_PROFILE
(the statement's end, which `sqlite3_reset()` does run) with the hook not
having run, polls the WAL reader with no frame hint (`walread.c`
`hpplite_walread_poll()`), which reads to the last commit frame. The poll
runs only for a statement that can have committed (autocommit, not
read-only): polling at the end of every statement cost 40 µs each
(`doc/performance.md`, Page capture).

**Pinned by.** Nothing, since the `test_abuse_pages` case `TEST("INSERT/UPDATE
... RETURNING stepped once and reset, then run again: every run captured")`
(11 commits captured, 2 without the fallback) went with test_abuse in #253.
The test it wants: the same case in test_pagemode - a `RETURNING` write
stepped once and reset, a sequencer that posts, a replica that equals.
The sequencer's timer poll would catch such a commit anyway within a tick,
so a regression shows as a late capture, not a lost one; `pending` would
lag the file until the tick.

## 2. A read transaction pinned at a WAL frame bounds every checkpoint there, and the frames past it can be cut off the WAL file

**Relied on (#155).** The WAL reader keeps a cursor into `<db>-wal` and
reads frames as they are committed; SQLite may checkpoint (backfill) frames
into the database and, once every frame is backfilled, restart the WAL from
its header with new salts, which would put new frames where unread ones
were. The sequencer keeps a read transaction open on one of two plain
connections of its own (`node.c` `pinDb[]`), at or before the reader's
cursor, and moves it forward right after each poll (the new pin is taken
before the poll, the old one let go after): SQLite backfills only up to
the oldest reader's mark and restarts the WAL only when all of it is
backfilled, so no frame the reader has not read is ever written over. The
pin costs WAL growth for as long as it is held and nothing else.

The same behaviour was spiked for the soft tier (#166) on 3.54.1 and trunk
3.55.0 (2026-11-02): a replica could keep soft batches discardable by
pinning at the hard height and truncating the WAL at the pin's frame on a
disagreement. The spike found that a partial checkpoint under the pin
copies a page only when its latest frame is within the safe range (the
main file alone is a mix, the WAL up to the pin is the hard state) and
that the wal-index is rebuilt only by the first opener with the exclusive
DMS lock. The shipped design rebuilds the replica from the chain and the
store instead (`node.c` `node_soft_rebuild()`), so only the sequencer's
pin relies on this entry today.

**SQLite source.** `wal.c` `walCheckpoint()`: `mxSafeFrame` is lowered to
each reader's `aReadMark[i]` the checkpoint cannot take the lock of, and
the copy loop skips `iFrame>mxSafeFrame` (over an iterator that yields each
page's last frame, hence the mix); `walRestartLog()` restarts the WAL only
when `pInfo->nBackfill==pWal->hdr.mxFrame`; `sqlite3WalClose()` runs no
checkpoint and deletes nothing when `sqlite3PagerClose()` passes it no
buffer, which `SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE` makes it do;
`walIndexRecover()` is run by `walIndexReadHdr()` when the connection holds
the exclusive DMS lock (`walLockShared`/`WAL_RECOVER_LOCK`), the WAL file's
salts and checksums deciding which frames are valid.

**HPPLite code.** `node.c` `node_pin_open()`, `node_wal_maintenance()` (the
new pin taken, the poll, the old pin let go; run from the timer each tick
and from the application's wal hook at 1000 frames), `node_start_pages()`
(the TRUNCATE checkpoint before the watch begins, so the file holds
everything the page mode reads), `walread.c` (the cursor, a WAL restart -
new salts after a checkpoint - starting it over).

**Pinned by.** test_wal_pin: `TEST("a reader pinned at a frame bounds every
checkpoint there: PASSIVE stops, TRUNCATE is BUSY, the pin still reads its
snapshot")`, `TEST("the frames past the pin cut off the WAL file: the
database reopens at the pin's state, intact, writable")`, `TEST("a pin held
under 3000 commits: the WAL grows, the autocheckpoint waits, the release
lets a TRUNCATE checkpoint take it all")`. test_pagemode `TEST("a thousand
commits, a checkpoint and a WAL restart by a connection without HPPLite,
with no tick of the sequencer between: nothing is lost")` pins what the pin
is for.

## 3. `SQLITE_FCNTL_DATA_VERSION` moves per own commit and per detected change

**Relied on.** The data version of a connection changes when that connection
commits, and when it first sees that another connection changed the file -
once per detection, not once per foreign commit. The bookkeeping connection
compares it with a fingerprint of HPPLite's own tables at the end of each
look: a version that moved while nothing of HPPLite's changed means
something else wrote the bookkeeping database (#37:
`hpplite_status.foreign_writes`, one line on stderr the first time).

With pages the bookkeeping connection is the sidecar's, which only HPPLite
writes, so the counter is rarely more than zero; the application's own
file is read from the WAL, where a plain connection's commits are captured
like any other.

**SQLite source.** `main.c` `sqlite3_file_control()`:
`SQLITE_FCNTL_DATA_VERSION` returns `sqlite3PagerDataVersion()` (`pager.c`),
which is `pPager->iDataVersion`. Incremented in `pager.c`
`sqlite3PagerCommitPhaseTwo()` (own commit) and `pager_reset()`, called from
`sqlite3PagerSharedLock()` when the file's change counter differs from the
one last read (rollback journal) and from `pagerBeginReadTransaction()`
when `sqlite3WalBeginReadTransaction()` reports a change (WAL).

**HPPLite code.** `bookkeeping.c` `bkForeignCheck()`, `bkFingerprint()`,
`hppliteBkEnd()` (the baseline after an own commit), `hppliteBkBegin()`
(`commitsDoneBefore`).

**Pinned by.** Nothing since #253 (test_rows_e2e
`test_foreign_writes_detected` went with row capture). The test it wants: a
write into the sidecar by a plain connection, counted once in
`foreign_writes`.

## Adding an entry

An entry has four parts: what HPPLite relies on and why it matters, the
SQLite source (file and function in `src/`, the `tag-` comment when one
exists; "not located" rather than a guess), the HPPLite code (file and
function), and the test that fails if it changes (suite and `TEST("...")`
line). A behaviour nothing pins is written up as such, with the test it
wants. The trunk job is the standing check that the list is complete: a
red run that no entry explains is a missing entry.
