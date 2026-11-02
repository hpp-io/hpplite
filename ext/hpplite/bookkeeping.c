/*
** HPPLite - the bookkeeping transaction.
**
** Reading the pending log, computing roots and finalizing batches happen on
** the bookkeeping connection, in a transaction that never lands inside the
** application's; on the shared (:memory:) connection only while the
** application is in autocommit. Its end is also where a foreign write (a
** process without HPPLite) is noticed (#37).
*/

#include "hpplite_int.h"
#include <string.h>
#include <unistd.h>
#include "state_tree.h"
#include <stdio.h>

/*
** Bookkeeping transactions. With a dedicated bookkeeping connection these
** never touch the application's transaction. On the shared (:memory:)
** connection they are only possible while the application is in autocommit.
*/
/* How long a bookkeeping write waits for the application to hand the gate
** over: a few of its transactions, not a timer tick */
#define HPPLITE_GATE_WAIT_US 20000

int hppliteBkBegin(HppliteCtx *pCtx, int bWrite) {
  int gated = bWrite && pCtx->bkDb != pCtx->db && !pCtx->bAppFlushing;
  int acquired = 0;
  if (pCtx->bkDb == pCtx->db && !sqlite3_get_autocommit(pCtx->db)) {
    return SQLITE_BUSY;
  }
  /* In WAL mode a connection that is inside a read transaction cannot
  ** start a write once another connection has committed (SQLITE_BUSY_SNAPSHOT,
  ** which the busy handler does not retry). A bookkeeping commit while the
  ** application is in the middle of a SELECT would make its next write on
  ** the same connection fail with "database is locked". So bookkeeping
  ** does not commit while an application statement is being stepped; the
  ** batch stays on L1 unfinalized and is adopted by a later flush. The
  ** application's own hpplite_flush() is exempt: that statement is the
  ** flush.
  **
  ** The same between statements of an application transaction: a read
  ** snapshot taken before our commit cannot become a write after it, so the
  ** transaction's next write would fail with "database is locked" (Knex's
  ** deferred BEGIN, Django's atomic(): read, then write). Even the
  ** application's own hpplite_flush() waits for its COMMIT.
  **
  ** With a bookkeeping connection of its own, the gate says all of that: the
  ** application holds it from a statement's start until no statement runs
  ** and no transaction is open. A write here takes the gate first and asks
  ** the application's state after. Only the call that takes the gate ever
  ** touches bBkGate, and only once its BEGIN went through: a begin that
  ** fails inside a transaction already open (the application thread asking
  ** for the state root while the timer thread finalizes, #133) must leave
  ** the holder's release alone, or the gate stays taken for good and every
  ** later statement spins on it. */
  if (gated) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&pCtx->txnGate, &expected, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      /* The application holds it. Ask for it and wait a little: it hands the
      ** gate over the next time it goes idle, the end of its transaction
      ** (#141); an application that never pauses between transactions would
      ** otherwise never leave a window to take it in. One that stays inside
      ** a transaction keeps it, and the batch is adopted by a later flush. */
      int i;
      __atomic_store_n(&pCtx->bBkWantsGate, 1, __ATOMIC_RELEASE);
      for (i = 0; i < HPPLITE_GATE_WAIT_US / 200 && __atomic_load_n(&pCtx->txnGate, __ATOMIC_ACQUIRE) != 2; i++) {
        expected = 0;
        if (__atomic_compare_exchange_n(&pCtx->txnGate, &expected, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
        usleep(200);
      }
      __atomic_store_n(&pCtx->bBkWantsGate, 0, __ATOMIC_RELEASE);
      if (__atomic_load_n(&pCtx->txnGate, __ATOMIC_ACQUIRE) != 2) {
        return SQLITE_BUSY;               /* still the application's (or held by a holder whose
                                          ** flag is not ours to clear) */
      }
    }
    acquired = 1;
  }
  if (bWrite && pCtx->bkDb != pCtx->db
      && ((!pCtx->bAppFlushing && pCtx->nAppStmtActive > 0) || pCtx->bAppTxnOpen)) {
    if (acquired) __atomic_store_n(&pCtx->txnGate, 0, __ATOMIC_RELEASE);
    return SQLITE_BUSY;
  }
  pCtx->commitsDoneBefore = __atomic_load_n(&pCtx->nCommitDone, __ATOMIC_ACQUIRE);   /* before this look's snapshot (#37) */
  {
    int rc = hppliteInternalExec(pCtx, pCtx->bkDb, bWrite ? "BEGIN IMMEDIATE" : "BEGIN");
    if (acquired) {
      if (rc == SQLITE_OK) pCtx->bBkGate = 1;
      else __atomic_store_n(&pCtx->txnGate, 0, __ATOMIC_RELEASE);
    }
    return rc;
  }
}

/*
** Writes past the log (#37): a connection opened without HPPLite (an ops
** person's sqlite3 shell) changes the file and leaves no log row. The
** sequencer's roots still match, it hashes the live file; only a rebuild
** shows the difference. Best effort, from inside a bookkeeping transaction:
** SQLite's data version moves when this connection commits or first sees
** another connection's commit - not once per foreign commit, so nothing
** can be counted; but if it moved while the application connection
** committed nothing and HPPLite's own tables (the log, the meta row) are as
** they were, something else wrote. Another HPPLite connection or process
** leaves log rows or a new height and is not counted.
**
** "Committed nothing" between two snapshots, exactly: the application's
** commits are counted at the commit hook, before they are visible, and
** again when the statement ends, after they are. Commits visible in a
** snapshot are at most the hook count read after it, and at least the
** done count read before it; equal counts across the two looks mean the
** same commits were visible in both.
*/
static sqlite3_int64 fnv(sqlite3_int64 h, const unsigned char *z, int n) {
  uint64_t u = (uint64_t)h;                      /* FNV-1a: unsigned arithmetic, the overflow is the point (#138) */
  int i;
  for (i = 0; i < n; i++) u = (u ^ z[i]) * 1099511628211ULL;
  return (sqlite3_int64)u;
}
static int bkFingerprint(HppliteCtx *pCtx, sqlite3_int64 out[3]) {
  sqlite3_stmt *q = NULL;
  int rc = sqlite3_prepare_v2(pCtx->bkDb, "SELECT min(id), max(id) FROM _hpplite_pending", -1, &q, NULL);
  if (rc != SQLITE_OK) return rc;
  out[0] = out[1] = out[2] = 0;
  if (sqlite3_step(q) == SQLITE_ROW) { out[0] = sqlite3_column_int64(q, 0); out[1] = sqlite3_column_int64(q, 1); }
  sqlite3_finalize(q);
  rc = sqlite3_prepare_v2(pCtx->bkDb, "SELECT * FROM _hpplite_meta", -1, &q, NULL);
  if (rc != SQLITE_OK) return rc;
  if (sqlite3_step(q) == SQLITE_ROW) {
    int i, n = sqlite3_column_count(q);
    sqlite3_int64 h = 1469598103934665603LL;
    for (i = 0; i < n; i++) {
      const unsigned char *z = sqlite3_column_blob(q, i);
      int len = sqlite3_column_bytes(q, i);
      h = fnv(h, (const unsigned char *)"|", 1);
      if (z) h = fnv(h, z, len);
    }
    out[2] = h;
  }
  sqlite3_finalize(q);
  return SQLITE_OK;
}
/* On the bookkeeping connection only, which has no hooks: bInternal is the
** application thread's and is left alone */
static void bkForeignCheck(HppliteCtx *pCtx, int doneBefore) {
  sqlite3_int64 f[3];
  unsigned dv = 0;
  if (bkFingerprint(pCtx, f) == SQLITE_OK) {
    int begunAfter = __atomic_load_n(&pCtx->nCommitBegun, __ATOMIC_ACQUIRE);   /* after this look's snapshot */
    sqlite3_file_control(pCtx->bkDb, "main", SQLITE_FCNTL_DATA_VERSION, &dv);
    if (pCtx->bBkDataVersionValid && dv != pCtx->bkDataVersion && begunAfter == pCtx->commitsDoneAtLook
        && memcmp(f, pCtx->aBkFinger, sizeof(f)) == 0) {
      pCtx->nForeignWrites++;
      if (pCtx->nForeignWrites == 1) {
        fprintf(stderr, "hpplite: the database changed with no HPPLite connection writing it: "
                        "something writes past the log (hpplite_status.foreign_writes); a rebuild from the chain will differ\n");
      }
    }
    memcpy(pCtx->aBkFinger, f, sizeof(f));
  }
  pCtx->commitsDoneAtLook = doneBefore;
}

#ifdef HPPLITE_TEST_HOOKS
int hpplite_test_fail_commits;   /* a test makes the next n finalize/adopt COMMITs fail as SQLITE_FULL (a full disk) */
long hpplite_test_bk_commits;    /* bookkeeping write transactions committed (#145) */
#endif
int hppliteBkEnd(HppliteCtx *pCtx, int rc) {
  int bLook = rc == SQLITE_OK && pCtx->bkDb != pCtx->db;
  if (bLook) bkForeignCheck(pCtx, pCtx->commitsDoneBefore);
  if (rc == SQLITE_OK) {
    rc = hppliteInternalExec(pCtx, pCtx->bkDb, "COMMIT");
#ifdef HPPLITE_TEST_HOOKS
    if (rc == SQLITE_OK && pCtx->bBkGate) hpplite_test_bk_commits++;
#endif
  }
  if (bLook && rc == SQLITE_OK) {
    /* the baseline: after our own commit (which moves the version), with
    ** the application's commits since the last look consumed - one that
    ** lands after our snapshot leaves log rows, and is attributed by them */
    sqlite3_file_control(pCtx->bkDb, "main", SQLITE_FCNTL_DATA_VERSION, &pCtx->bkDataVersion);
    pCtx->bBkDataVersionValid = 1;
  }
  if (rc != SQLITE_OK) {
    hppliteInternalExec(pCtx, pCtx->bkDb, "ROLLBACK");
    hppliteTreeRolledBack(pCtx);
  }
  if (pCtx->bBkGate) {
    pCtx->bBkGate = 0;
    __atomic_store_n(&pCtx->txnGate, 0, __ATOMIC_RELEASE);
  }
  return rc;
}
