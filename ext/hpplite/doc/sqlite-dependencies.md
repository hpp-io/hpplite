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
the ctest suite and the test case's `TEST("...")` line; `_text` suites are
the same binary with `HPPLITE_CAPTURE=text`.

## Summary

| # | Behaviour | Pinned by | Trunk 2026-10-31 |
|---|---|---|---|
| 1 | A stale statement is reprepared and rerun inside one `sqlite3_step()`, with no STMT trace and the interrupt cleared | test_rows, test_concurrency(+_text) | **broken** (cause: 10) |
| 2 | PROFILE runs before the result code is recorded | nothing pins it directly | ok |
| 3 | A statement run from a trace callback joins the running statement's transaction | test_rows, test_concurrency(+_text) | ok |
| 4 | With a read transaction open, a write's lock attempt does not call the busy handler | test_rows_e2e | ok (its test fails on trunk, cause: 10) |
| 5 | The connection's error code is SQLITE_SCHEMA while SQLite reprepares a stale statement | test_rows | ok |
| 6 | `sqlite3_expired()` is set by a stale first run and by a rollback that undid a schema change | test_rows | ok |
| 7 | `SQLITE_FCNTL_DATA_VERSION` moves per own commit and per detected change, not per foreign commit | test_rows_e2e | ok |
| 8 | The rollback hook fires for the implicit rollback of a failed autocommit statement | test_hpplite | ok |
| 9 | `sqlite3_expanded_sql()` truncates bound values in some builds | test_env_enable | ok |
| 10 | A schema reset is immediate even while another statement of the connection is running | test_rows, test_concurrency(+_text), test_faults(+_text), test_rows_e2e | handled (#108) |

Entry 10 was not on the list in #105: it is the one trunk broke, found by the
first run of the trunk build (2026-11-01; details under 10). HPPLite no
longer relies on it (#108): the capture of a statement whose start finds the
schema stale and pinned is deferred to its first run's end.

## 1. A stale statement is reprepared and rerun inside the same `sqlite3_step()`

**Relied on.** A statement prepared on a schema another connection then
changed fails its first run with SQLITE_SCHEMA. `sqlite3_step()` reprepares
it and runs it again, from the top, inside the same call: the application
sees one step. The rerun raises no SQLITE_TRACE_STMT event, and it starts
with the interrupt flag cleared. So HPPLite cannot log the rerun from a
trace (there is none), and a refusal by `sqlite3_interrupt()` of the first
run does not reach the rerun (#89). In autocommit the failed first run's
rollback takes HPPLite's log row with it (entry 3), so the rerun would go
unlogged; HPPLite instead opens a transaction of its own before the
statement's own begins, and commits it at the statement's end (#95).

**SQLite source.** `vdbeapi.c` `sqlite3_step()`: the `while( (rc =
sqlite3Step(v))==SQLITE_SCHEMA && cnt++ < SQLITE_MAX_SCHEMA_RETRY )` loop
calls `sqlite3Reprepare()` (`prepare.c`) and sets `v->minWriteFileFormat =
254` with the comment `tag-20220401a`; `vdbe.c` `OP_Trace`/`OP_Init` skips
the STMT trace when `p->minWriteFileFormat==254` (the same tag). The
interrupt flag is cleared in `vdbeapi.c` `sqlite3Step()`: `if(
db->nVdbeActive==0 ){ AtomicStore(&db->u1.isInterrupted, 0); }` as the VM
starts. SQLITE_SCHEMA itself is raised in `vdbe.c` `OP_Transaction` when the
schema cookie or generation differs from the one compiled in (`p->expired =
1; rc = SQLITE_SCHEMA`).

**HPPLite code.** `capture.c`: `rerunTrack()`, `rerunBegin()`,
`rerunCommit()`, `rerunCommitRetry()`, `rerunStatementDone()`; the comment
above `rerunTrack()` names `tag-20220401a`. The refusal's carry-over to the
rerun: `hppliteAuthorizer()` (`pCtx->pRefusedStmt`) and the PROFILE branch
of `hppliteTraceCallback()` (`if (pS == pCtx->pRefusedStmt &&
!stmtExpired(pS))`). The rerun that never comes: the STMT branch of
`hppliteTraceCallback()` (`rerunStage == 2`, #95).

**Pinned by.** test_concurrency and test_concurrency_text:
`test_rerun_after_foreign_schema_change` ("a statement SQLite reruns after
another connection's schema change is still captured") and
`test_rerun_in_transaction` ("the same inside the application's
transaction: the rerun is still captured"). test_rows:
`test_refused_stale_statement` ("a refused statement that is also stale
does not run on its rerun", #89) and `test_tracked_schema_change_commits`
("a tracked statement that changes the schema itself commits at its end;
nothing stays open", #95).

## 2. The PROFILE callback runs before the result code is recorded

**Relied on.** From SQLITE_TRACE_PROFILE a statement's error is not
observable: `sqlite3_errcode()` still holds the previous value. HPPLite does
not try; it judges a logged statement by its effect (rows changed, schema
version, `sqlite_sequence`) and removes the row of one that had none
(`hppliteStatementDone()`).

**SQLite source.** `vdbeapi.c` `sqlite3Step()`: after `sqlite3VdbeExec()`
returns, `checkProfileCallback(db, p)` (which calls
`invokeProfileCallback()`, the SQLITE_TRACE_PROFILE event) runs first; the
error is transferred afterwards, by `sqlite3VdbeTransferError()`
(`vdbeaux.c`) in the `rc!=SQLITE_DONE` branch and then `db->errCode = rc`.

**HPPLite code.** `capture.c` `hppliteStatementDone()` and the comment
above it ("the profile callback runs before SQLite records the result
code"); called from the PROFILE branch of `hppliteTraceCallback()`.

**Pinned by.** Nothing pins the order directly, and a change in the
"better" direction (the error visible from PROFILE) would break no test:
HPPLite would keep judging by effect. What is pinned is the effect check
itself: test_rows `test_failures_and_rollbacks` ("failed statements and
rolled-back transactions leave no records; changes() and
last_insert_rowid() as plain SQLite"), test_durability and
test_durability_text `test_replay_skips_failed_statement` ("statement
failed at sequencer is skipped on replay, content identical"), test_abuse
and test_abuse_text "batches built while no-op statements commit and unlog:
adoption never mismatches". Proposed pin, if one is wanted: a PROFILE
callback registered after HPPLite's (test only) that records
`sqlite3_errcode()` for a failing INSERT and asserts it is not yet
SQLITE_CONSTRAINT; it documents the order rather than guards HPPLite.

## 3. A statement run from a trace callback joins the running statement's transaction

**Relied on.** HPPLite's log INSERT (and, under row capture, the lock
no-op `PRAGMA main.incremental_vacuum(0)`, a write that changes nothing and leaves the change count alone, #121) runs from the STMT
trace of the application's write, while that write's VM is active. In
autocommit SQLite does not commit HPPLite's statement on its own: it joins
the transaction the application's statement opened, and commits or rolls
back with it. The log row and the write are atomic; a failed autocommit
statement takes its own log row away.

**SQLite source.** `vdbeaux.c` `sqlite3VdbeHalt()`: the autocommit
commit/rollback block runs only when `db->autoCommit &&
db->nVdbeWrite==(p->readOnly==0)`, that is when the halting VM is the only
active writer. `db->nVdbeWrite` is incremented in `vdbeapi.c`
`sqlite3Step()` as a non-read-only VM starts and decremented at the end of
`sqlite3VdbeHalt()`. While the application's write VM is active HPPLite's
statement halts with `nVdbeWrite==2`, so nothing is committed; the
transaction ends with the application's statement.

**HPPLite code.** `capture.c` `hppliteTraceCallback()` (the STMT branch:
`pCtx->pLogStmt` stepped by `hpplite_step_write()`), `rerunTrack()` (the
schema read "in the write transaction"), `hppliteStatementDone()` ("In
autocommit a failed statement's implicit transaction already rolled the row
back"); `capture_rows.c` `hpplite_rows_covered_for_write()` ("in the
statement's own transaction: it is running").

**Pinned by.** test_rows `test_failures_and_rollbacks` (a UNIQUE failure in
autocommit leaves no record; replica equal); test_concurrency and
test_concurrency_text `test_rerun_after_foreign_schema_change` (the failed
first run rolls the row back and the rerun is logged again: both halves of
this entry). A statement-level pin for text capture (a failed autocommit
INSERT leaves `_hpplite_pending` as it was) does not exist as its own case;
test_hpplite "only replayable writes are logged" counts rows for successful
statements only.

## 4. With a read transaction open, a write's lock attempt does not call the busy handler

**Relied on.** A connection inside a read transaction (a snapshot) that
tries to write while another connection holds the write lock gets
SQLITE_BUSY at once (SQLITE_BUSY_SNAPSHOT when the snapshot is stale): the
busy handler is not invoked, because waiting could not help - the snapshot
could never be upgraded. HPPLite's #92 design rests on it: under row
capture the lock no-op waits once (busy_timeout), and when it fails
HPPLite opens a snapshot (`rerunTrack()`'s schema read) and lets the
application's statement take the lock itself; SQLite does not wait a second
time, and the driver's retry comes back through the trace.

**SQLite source.** `btree.c` `btreeBeginTrans()`: the retry loop `}while(
(rc&0xFF)==SQLITE_BUSY && pBt->inTransaction==TRANS_NONE &&
btreeInvokeBusyHandler(pBt) );` - the busy handler is only consulted when no
transaction is open on the shared b-tree; with one open, SQLITE_BUSY (or
SQLITE_BUSY_SNAPSHOT from `sqlite3PagerBegin()`, turned into SQLITE_BUSY
only when `inTransaction==TRANS_NONE`) returns at once.

**HPPLite code.** `capture_rows.c` `hpplite_rows_covered_for_write()`
(comment: "SQLITE_BUSY at once (no busy handler)", the `HPPLITE_ROWS_BUSY`
return); `capture.c` `hppliteTraceCallback()` STMT branch (`covered ==
HPPLITE_ROWS_BUSY`, #92) and `hppliteBkBegin()` (the SQLITE_BUSY_SNAPSHOT comment:
bookkeeping never commits under an application statement).

**Pinned by.** test_rows_e2e `test_lock_contention` ("write lock held
elsewhere past busy_timeout: one wait and SQLITE_BUSY (in a transaction
too, after a schema change too, never a refusal); retried, random() and the
write under the new schema are rows", #92): it counts the busy-handler
waits with its own handler and asserts one, not two. test_abuse and
test_abuse_text "read-then-write transactions never hit 'database is
locked' while flushes run" and "a flush during an application's
read-then-write transaction does not lock it out" pin the `hppliteBkBegin()` side.

## 5. The connection's error code is SQLITE_SCHEMA while SQLite reprepares a stale statement

**Relied on.** HPPLite's authorizer tells the reprepare of a refused stale
statement (entry 1: no trace, interrupt cleared, so it must be denied at
prepare) from the application's next prepare by `sqlite3_errcode(db) ==
SQLITE_SCHEMA`: true only inside `sqlite3_step()`'s reprepare, never for a
prepare the application makes (#101).

**SQLite source.** `vdbeapi.c` `sqlite3Step()`: when the first run fails,
`sqlite3VdbeTransferError()` (`vdbeaux.c`) sets `db->errCode = p->rc`
(SQLITE_SCHEMA) and `db->errCode = rc` follows; `sqlite3_step()` then calls
`sqlite3Reprepare()` -> `sqlite3LockAndPrepare()` -> `sqlite3Prepare()`
(`prepare.c`), which does not reset `db->errCode` before the parser runs
the authorizer (`auth.c` `sqlite3AuthCheck()` -> `db->xAuth`). The
application's own prepare after a failed step follows a reset or
finalize, which leaves the statement's own code in `db->errCode`, or a new
statement's prepare, which ends with its own code.

**HPPLite code.** `capture.c` `hppliteAuthorizer()`: `int bReprepare =
sqlite3_errcode(pCtx->db) == SQLITE_SCHEMA;` under `pCtx->pRefusedStmt`.

**Pinned by.** test_rows `test_refused_stale_statement` (the reprepare is
denied), `test_refused_then_expired` ("a refused statement expired by its
rollback (a schema change in the transaction) leaves the next statement
allowed", #101) and "a statement refused after another connection's ALTER
(stale triggers, text rules) leaves the next statement allowed" (the
application's next prepare is not taken for the reprepare).

## 6. `sqlite3_expired()` is set by a stale first run and by a rollback that undid a schema change

**Relied on.** After the first run fails stale, `sqlite3_expired()` is true
at its PROFILE: HPPLite reads it there to tell "the rerun follows" from
"it is over" (`rerunStatementDone()`, the `pRefusedStmt` carry-over). It is
also true when the statement's own interrupt rolled back a transaction with
a schema change in it, which expires every statement - and then no rerun
follows (#101), so `expired` alone is not the signal: the schema version is
compared too. `sqlite3_expired()` is a deprecated interface; a SQLite built
with SQLITE_OMIT_DEPRECATED has none, and the loadable build assumes
"expired" then (the commit at the rerun's end or the next statement's start
covers it).

**SQLite source.** `vdbeapi.c` `sqlite3_expired()` returns `p->expired`
(under `#ifndef SQLITE_OMIT_DEPRECATED`). Set by `vdbe.c` `OP_Transaction`
(`p->expired = 1` with SQLITE_SCHEMA) and by `main.c` `sqlite3RollbackAll()`:
`if( schemaChange ){ sqlite3ExpirePreparedStatements(db, 0); ... }`
(`vdbeaux.c` `sqlite3ExpirePreparedStatements()` sets `expired` on every
statement of the connection).

**HPPLite code.** `capture.c` `stmtExpired()`, `rerunStatementDone()`, the
PROFILE branch of `hppliteTraceCallback()`.

**Pinned by.** test_rows `test_refused_stale_statement` (expired by a stale
run: the rerun is refused), `test_refused_then_expired` (expired by the
rollback: no rerun, the next statement allowed, #101),
`test_tracked_schema_change_commits` (expired by its own nested DDL: it
ran, commit at its end, #95). test_loadable (the module built as an
extension, where `sqlite3_api->expired` may be absent).

## 7. `SQLITE_FCNTL_DATA_VERSION` moves per own commit and per detected change

**Relied on.** The data version of a connection changes when that connection
commits, and when it first sees that another connection changed the file -
once per detection, not once per foreign commit. So it cannot count foreign
writes; but a version that moved while the application committed nothing
and HPPLite's own tables are as they were means something wrote past the
log (#37: `hpplite_status.foreign_writes`). The function-verdict cache key
also uses it, because it is in memory (no read of the file under a running
statement).

**SQLite source.** `main.c` `sqlite3_file_control()`:
`SQLITE_FCNTL_DATA_VERSION` returns `sqlite3PagerDataVersion()` (`pager.c`),
which is `pPager->iDataVersion`. Incremented in `pager.c`
`sqlite3PagerCommitPhaseTwo()` (own commit) and `pager_reset()`, called from
`sqlite3PagerSharedLock()` when the file's change counter differs from the
one last read (rollback journal) and from `pagerBeginReadTransaction()`
when `sqlite3WalBeginReadTransaction()` reports a change (WAL).

**HPPLite code.** `bookkeeping.c` `bkForeignCheck()`, `hppliteBkEnd()` (the baseline
after an own commit), `hppliteBkBegin()` (`commitsDoneBefore`), the PROFILE branch
of `hppliteTraceCallback()` (`nCommitDone`, `capture.c`), `hppliteCommitHook()`
(`nCommitBegun`); the comment above `fnv()` ("Writes past the log (#37)");
`functionVerdict()` (`capture.c`, the cache key).

**Pinned by.** test_rows_e2e `test_foreign_writes_detected` ("a write by a
connection without HPPLite is counted in hpplite_status.foreign_writes;
HPPLite's own traffic is not", #37).

## 8. The rollback hook fires for the implicit rollback of a failed autocommit statement

**Relied on.** When an autocommit statement fails after it began writing,
SQLite rolls its transaction back and calls the rollback hook, as it does
for an explicit ROLLBACK. HPPLite's hook resets the current transaction
group (`curTxn = 0`) and forwards to `xOnRollback`. Without it the group id
would stay that of the rolled-back log row, and the next autocommit
statement's row would name a group whose first row does not exist.

**SQLite source.** `vdbeaux.c` `sqlite3VdbeHalt()`: in the autocommit
block, `p->rc != SQLITE_OK` leads to `sqlite3RollbackAll(db, SQLITE_OK)`;
`main.c` `sqlite3RollbackAll()` ends with `if( db->xRollbackCallback &&
(inTrans || !db->autoCommit) ){ db->xRollbackCallback(db->pRollbackArg); }`
- `inTrans` is set when a b-tree was in SQLITE_TXN_WRITE, which a statement
that began writing is.

**HPPLite code.** `capture.c` `hppliteRollbackHook()`; installed in
`hpplite.c` `hpplite_init_ex()` and re-armed by `rearmHooks()`.

**Pinned by.** test_hpplite `test_rollback_hook_failed_autocommit` (the suite
calls `hpplite_init()`: statement capture; the hook is the same code under
row capture) ("a failed autocommit statement fires
the rollback hook; the next row starts its own group", #109): after a
successful autocommit INSERT, one that fails a UNIQUE constraint, then
another successful one, the `on_rollback` callback has fired once and the
last `_hpplite_pending` row has `txn = 0` (a group of its own). Without the
hook's `curTxn = 0` the row joins the rolled-back row's group ("group 3,
want 0").

## 9. `sqlite3_expanded_sql()` truncates bound values in some builds

**Relied on.** Text capture inlines bound values with
`sqlite3_expanded_sql()`. A SQLite compiled with SQLITE_TRACE_SIZE_LIMIT
(better-sqlite3 bundles one with 32) cuts every longer string or blob in
the expansion, and a replica would replay the cut value. SQLite has no
other way to read bound values back. HPPLite probes once at init with a
long bound string: under text capture such a build is refused at open;
under row capture it is accepted and the few statements still logged as
text are refused when they bind anything (#62).

**SQLite source.** `vdbetrace.c` `sqlite3VdbeExpandSql()`: `#ifdef
SQLITE_TRACE_SIZE_LIMIT ... if( nOut>SQLITE_TRACE_SIZE_LIMIT ){ nOut =
SQLITE_TRACE_SIZE_LIMIT; ...` for text and blob values. `vdbeapi.c`
`sqlite3_expanded_sql()` is the public entry.

**HPPLite code.** `hpplite.c` `expandedSqlIsFaithful()`, `hpplite_init_ex()`
(`bTruncates`, `gInitError`), the STMT branch of `hppliteTraceCallback()`
(`pCtx->bExpandTruncates`, `capture.c`); `node.c` `node_init_tracking()` (row capture
passes `!captureText`).

**Pinned by.** test_env_enable `test_truncating_sqlite_refused` ("a SQLite
that truncates bound values in expanded SQL (better-sqlite3): statement
capture is refused at open") and `test_truncating_sqlite_rows` ("the same
SQLite with row capture: opens, records long bound values whole, refuses a
bound text-logged write", #62). Both simulate the build with
`HPPLITE_TEST_TRUNCATING_EXPAND` (test hooks); no CI build defines
SQLITE_TRACE_SIZE_LIMIT itself.

## 10. A schema reset is immediate even while another statement of the connection is running

**Relied on.** HPPLite's own prepared statements (the log INSERT
`pLogStmt`, the schema read `pSchemaStmt`, the lock no-op `pRowsLockStmt`,
all SQLITE_PREPARE_PERSISTENT) run from the STMT trace of the application's
statement, while that statement is active. When another connection changed
the schema, HPPLite's statement is stale too: its `OP_Transaction` sees the
new cookie, resets the in-memory schema, and `sqlite3_step()` reprepares it
against the schema reloaded from the file - the nested reprepare succeeds
even though the application's VM, compiled on the old schema, is still
running. Entries 1, 3, 4 and 5 all pass through this nested reprepare.

**SQLite source.** Up to 3.54.1: `build.c` `sqlite3ResetOneSchema(db,
iDb)` clears the schema whenever `db->nSchemaLock==0`, regardless of active
VMs; `vdbe.c` `OP_Transaction` calls it on a cookie mismatch. On trunk
since the check-ins of 2026-10-26/27 (`c1a6ef0daa` "Do not be as aggressive
at blocking automatic reprepares due to a pending schema reset",
`031a3911c3` "A more accurate approach to resetting schemas on active
statements", `6ca068cdce` "Expire every prepared statement that uses a
schema when that schema is cleared"): `sqlite3ResetOneSchema(db, iDb,
pSafe)` consults `sqlite3VdbeSchemaInUse()` (`vdbeaux.c`) and defers the
reset (`db->bPendingSchemaReset`) while any VM other than `pSafe` in
VDBE_RUN_STATE uses that schema; the deferred reset runs at the next
`sqlite3VdbeHalt()` with `db->nVdbeActive==0`. HPPLite's nested statement
is reprepared against the unreset, stale schema, fails SQLITE_SCHEMA again,
and after SQLITE_MAX_SCHEMA_RETRY attempts `sqlite3_step()` returns
SQLITE_SCHEMA ("database schema has changed").

**HPPLite code.** `capture.c` `hppliteTraceCallback()` STMT branch: the
`hpplite_step_write(pCtx->pLogStmt)` whose failure refuses the statement
("could not log statement (database schema has changed)"); `hppliteSchemaVersion()`
(`pSchemaStmt`); `capture_rows.c` `hpplite_rows_covered_for_write()`
(`pRowsLockStmt`), `schemaVersionOf()`.

**Pinned by.** Found by the first local run of the trunk build
(2026-11-01, trunk 3.55.0 `bda12170867d` of 2026-10-31; the same suites
pass against 3.54.1): test_concurrency and test_concurrency_text
`test_rerun_after_foreign_schema_change` and `test_rerun_in_transaction`;
test_rows "a table created by another connection: the first write to it is
logged as text, then rows" and `test_tracked_schema_change_commits`;
test_rows_e2e `test_lock_contention`; test_faults and test_faults_text (the
two-connection seeds).

**No longer relied on (#108).** When a statement of HPPLite's fails
SQLITE_SCHEMA at an application statement's start (the lock no-op or the
log INSERT: `HPPLITE_ROWS_STALE`, `deferCapture()` in `capture.c`), the
application's statement is stale too: HPPLite opens its transaction
(`rerunTrack()`), lets the first run fail at its own cookie check - which
resets the schema, this statement being the only one running - and does the
capture at that run's end (`rerunStatementDone()` → `captureStart()`,
triggers rebuilt there for a write), before SQLite reprepares and reruns it
in the same step. Under a held lock (#92) a probe (`hppliteSchemaProbe()`)
tells "stale and pinned" from "lock held"; a statement that then fails
SQLITE_BUSY leaves the rebuild owed, done under the retry's own lock with
the schema cookie moved on so the retry reruns with the new triggers rather
than aborting (a running statement expired by trigger DDL aborts at its
first cursor, SQLITE_ABORT_ROLLBACK, on every SQLite). The text rules are
judged at the start when the statement is text in any case, so a refusal
opens no transaction. On 3.54.1 the deferred paths are never taken (the
nested reprepare works and the lock no-op never fails SQLITE_SCHEMA); the
trunk CI job exercises them.

## Adding an entry

An entry has four parts: what HPPLite relies on and why it matters, the
SQLite source (file and function in `src/`, the `tag-` comment when one
exists; "not located" rather than a guess), the HPPLite code (file and
function), and the test that fails if it changes (suite and `TEST("...")`
line). A behaviour nothing pins is written up as such, with the test it
wants. The trunk job is the standing check that the list is complete: a
red run that no entry explains is a missing entry.
