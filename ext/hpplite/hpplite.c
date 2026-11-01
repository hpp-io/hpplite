/*
** HPPLite - Verifiable SQLite
**
** Core: statement capture into _hpplite_pending, batch building, content-hash
** state roots, replay from L1. See hpplite.h for the persistence model.
*/

#include "hpplite_int.h"
#include <ctype.h>
#include <fnmatch.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>

#include <time.h>

/*
** ============================================================================
** Local persistence
** ============================================================================
*/

static const char *HPPLITE_SCHEMA =
  "CREATE TABLE IF NOT EXISTS _hpplite_meta("
  "  id INTEGER PRIMARY KEY CHECK(id=1),"
  "  next_height INTEGER NOT NULL,"
  "  prev_block_hash BLOB NOT NULL,"
  "  state_root BLOB NOT NULL,"
  "  last_checkpoint_height INTEGER NOT NULL DEFAULT 0,"
  "  last_checkpoint_root BLOB,"
  "  open_txn INTEGER NOT NULL DEFAULT 0,"  /* writer: group left open by the last posted batch (#57) */
  "  frag_id INTEGER NOT NULL DEFAULT 0,"   /* writer: log row whose text is posted in part (#59) */
  "  frag_off INTEGER NOT NULL DEFAULT 0,"  /* ...bytes of it posted */
  "  rollup TEXT NOT NULL DEFAULT ''"       /* "<chain id>:<contract>" its history is on */
  ");"
  "INSERT OR IGNORE INTO _hpplite_meta(id, next_height, prev_block_hash, state_root)"
  "  VALUES(1, 1, zeroblob(32), zeroblob(32));"
  /* the bytes of the writer's recent batches, to post again if a chain
  ** reorganisation drops them (#49) */
  "CREATE TABLE IF NOT EXISTS _hpplite_posted("
  "  height INTEGER PRIMARY KEY,"
  "  data BLOB NOT NULL"
  ");"
  /* the part of a transaction a replica holds back until its end is posted (#57) */
  "CREATE TABLE IF NOT EXISTS _hpplite_carry("
  "  seq INTEGER PRIMARY KEY,"
  "  sql TEXT NOT NULL,"
  "  mode INTEGER NOT NULL DEFAULT 0,"
  "  now INTEGER NOT NULL DEFAULT 0,"
  "  frag INTEGER NOT NULL DEFAULT 0"
  ");"
  "CREATE TABLE IF NOT EXISTS _hpplite_pending("
  "  id INTEGER PRIMARY KEY,"
  "  txn INTEGER NOT NULL,"
  "  sql TEXT NOT NULL,"
  "  mode INTEGER NOT NULL DEFAULT 0,"  /* HPPLITE_MODE_*: the connection state it ran under (#52) */
  "  now INTEGER NOT NULL DEFAULT 0"    /* the time it read, Julian day ms; 0 = none (#56) */
  ");";

/*
** bInternal tells the trace and commit hooks on the application connection
** to ignore HPPLite's own statements. Only statements that run on that
** connection need it: the bookkeeping connection has no hooks, and the
** timer thread must not touch a flag the application thread is reading
** (that silently dropped captures, issue #20). bkEnter() sets the flag only
** when bookkeeping shares the application connection (:memory:).
*/
static int bkEnter(HppliteCtx *pCtx) {
  int saved = pCtx->bInternal;
  if (pCtx->bkDb == pCtx->db) pCtx->bInternal = 1;
  return saved;
}
static void bkLeave(HppliteCtx *pCtx, int saved) {
  if (pCtx->bkDb == pCtx->db) pCtx->bInternal = saved;
}

/*
** Run SQL on behalf of HPPLite itself on the given connection.
*/
int hppliteInternalExec(HppliteCtx *pCtx, sqlite3 *db, const char *zSql) {
  int saved = pCtx->bInternal;
  int rc;
  if (db == pCtx->db) pCtx->bInternal = 1;
  rc = sqlite3_exec(db, zSql, NULL, NULL, NULL);
  if (db == pCtx->db) pCtx->bInternal = saved;
  return rc;
}

static void columnHash(sqlite3_stmt *pStmt, int i, unsigned char *pOut) {
  const void *p = sqlite3_column_blob(pStmt, i);
  int n = sqlite3_column_bytes(pStmt, i);
  if (p && n == HPPLITE_HASH_SIZE) {
    memcpy(pOut, p, HPPLITE_HASH_SIZE);
  } else {
    memset(pOut, 0, HPPLITE_HASH_SIZE);
  }
}

/*
** Load the _hpplite_meta row and the transaction counter into the context.
*/
static int loadMeta(HppliteCtx *pCtx);
/*
** A follower's copy of the meta row is from its open; the sequencer moves
** the file. Before reporting (hpplite_status, the height API) a follower
** looks at the file's height and reloads when it moved (#100). One row.
*/
void hpplite_refresh_meta(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int saved;
  if (!pCtx || !pCtx->bkDb) return;
  saved = bkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT next_height FROM _hpplite_meta WHERE id=1", -1, &pStmt, NULL) == SQLITE_OK) {
    int moved = sqlite3_step(pStmt) == SQLITE_ROW && (uint64_t)sqlite3_column_int64(pStmt, 0) != pCtx->blockHeight;
    sqlite3_finalize(pStmt);
    if (moved) { bkLeave(pCtx, saved); loadMeta(pCtx); return; }
  }
  bkLeave(pCtx, saved);
}

static int loadMeta(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int rc;
  int saved = bkEnter(pCtx);
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT next_height, prev_block_hash, state_root,"
    "       last_checkpoint_height, last_checkpoint_root, open_txn,"
    "       (SELECT count(*) FROM _hpplite_carry), frag_id, frag_off, rollup"
    "  FROM _hpplite_meta WHERE id=1", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) {
      pCtx->blockHeight = (uint64_t)sqlite3_column_int64(pStmt, 0);
      columnHash(pStmt, 1, pCtx->prevBlockHash);
      columnHash(pStmt, 2, pCtx->stateRoot);
      pCtx->lastCheckpointHeight = (uint64_t)sqlite3_column_int64(pStmt, 3);
      columnHash(pStmt, 4, pCtx->lastCheckpointRoot);
      pCtx->openTxnId = sqlite3_column_int64(pStmt, 5);
      pCtx->nCarry = sqlite3_column_int(pStmt, 6);
      pCtx->fragId = sqlite3_column_int64(pStmt, 7);
      pCtx->fragOff = (size_t)sqlite3_column_int64(pStmt, 8);
      snprintf(pCtx->zRollup, sizeof(pCtx->zRollup), "%s",
               sqlite3_column_text(pStmt, 9) ? (const char*)sqlite3_column_text(pStmt, 9) : "");
    }
    sqlite3_finalize(pStmt);
  }

  bkLeave(pCtx, saved);
  pCtx->bRootDirty = 1;
  return rc;
}

void hpplite_error(HppliteCtx *pCtx, const char *zFmt, ...) {
  char z[512];
  va_list ap;
  va_start(ap, zFmt);
  vsnprintf(z, sizeof z, zFmt, ap);
  va_end(ap);
  fprintf(stderr, "hpplite: %s\n", z);
  if (pCtx) snprintf(pCtx->zLastError, sizeof(pCtx->zLastError), "%s", z);
}
void hpplite_error_clear(HppliteCtx *pCtx) {
  if (pCtx) pCtx->zLastError[0] = 0;
}

/*
** ============================================================================
** Lifecycle
** ============================================================================
*/

/* HPPLite's own connections to a database file (bookkeeping) must not be
** taken for the application's when HPPLITE_FILES names the file */
static _Thread_local int tlInternalOpen;
void hpplite_internal_open_begin(void) { tlInternalOpen++; }
void hpplite_internal_open_end(void) { tlInternalOpen--; }
int hpplite_internal_open_active(void) { return tlInternalOpen > 0; }

static int addColumnIfMissing(HppliteCtx *pCtx, sqlite3 *db, const char *zTable, const char *zCol) {
  sqlite3_stmt *q = NULL;
  int has = 0, rc = SQLITE_OK;
  char *z = sqlite3_mprintf("SELECT 1 FROM pragma_table_info(%Q) WHERE name = %Q", zTable, zCol);
  if (!z) return SQLITE_NOMEM;
  if (sqlite3_prepare_v2(db, z, -1, &q, NULL) == SQLITE_OK) has = (sqlite3_step(q) == SQLITE_ROW);
  sqlite3_finalize(q);
  sqlite3_free(z);
  if (!has) {
    z = sqlite3_mprintf("ALTER TABLE %s ADD COLUMN %s INTEGER NOT NULL DEFAULT 0", zTable, zCol);
    if (!z) return SQLITE_NOMEM;
    rc = hppliteInternalExec(pCtx, db, z);
    sqlite3_free(z);
  }
  return rc;
}

/*
** Capture inlines bound values with sqlite3_expanded_sql(). A SQLite built
** with SQLITE_TRACE_SIZE_LIMIT (better-sqlite3's bundled one: 32) cuts every
** longer string or blob there, and the replica would replay the cut value -
** silently different data. SQLite has no other way to read bound values
** back, so such a build cannot be used: found once, at init.
*/
static const char *gInitError;           /* why the last hpplite_init failed, for the open's message */
const char *hpplite_init_error(void) { return gInitError; }

static int expandedSqlIsFaithful(sqlite3 *db) {
  static const char zLong[] = "hpplite-probe-0123456789abcdefghijklmnopqrstuvwxyz-0123456789abcdefghijklmnopqrstuvwxyz";
  sqlite3_stmt *st = NULL;
  char *z;
  int ok = 0;
#ifdef HPPLITE_TEST_HOOKS
  if (getenv("HPPLITE_TEST_TRUNCATING_EXPAND")) return 0;
#endif
  if (sqlite3_prepare_v2(db, "SELECT ?1", -1, &st, NULL) != SQLITE_OK) return 0;
  sqlite3_bind_text(st, 1, zLong, -1, SQLITE_STATIC);
  z = sqlite3_expanded_sql(st);
  ok = z && strstr(z, zLong) != NULL;
  sqlite3_free(z);
  sqlite3_finalize(st);
  return ok;
}

int hpplite_init(sqlite3 *db, HppliteCtx **ppCtx) { return hpplite_init_ex(db, ppCtx, 0); }

int hpplite_init_ex(sqlite3 *db, HppliteCtx **ppCtx, int bRowsCapture) {
  HppliteCtx *pCtx;
  int rc, bTruncates;

  gInitError = NULL;
  bTruncates = !expandedSqlIsFaithful(db);
  if (bTruncates && !bRowsCapture) {
    gInitError = "this SQLite truncates bound values in sqlite3_expanded_sql() (built with "
                 "SQLITE_TRACE_SIZE_LIMIT, as better-sqlite3's bundled SQLite is): statement capture "
                 "would replay cut strings. Use row capture (the default; not capture=text), or a "
                 "driver on a stock SQLite (Node: node:sqlite or the sqlite3 package; Python: sqlite3)";
    fprintf(stderr, "hpplite: %s\n", gInitError);
    return SQLITE_ERROR;
  }

  pCtx = (HppliteCtx*)sqlite3_malloc(sizeof(HppliteCtx));
  if (!pCtx) return SQLITE_NOMEM;
  memset(pCtx, 0, sizeof(HppliteCtx));
  pCtx->bExpandTruncates = bTruncates;
  pCtx->seenSchema = -1;

  pCtx->db = db;
  pCtx->bkDb = db;
  pCtx->bEnabled = 1;

  rc = hppliteInternalExec(pCtx, db, HPPLITE_SCHEMA);
  if (rc == SQLITE_OK) {
    /* files from before #52: the log has no mode column yet (rows already
    ** in it ran with the default state, which DEFAULT 0 records) */
    sqlite3_stmt *q = NULL;
    int has = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info('_hpplite_pending') WHERE name = 'mode'",
                           -1, &q, NULL) == SQLITE_OK) {
      has = (sqlite3_step(q) == SQLITE_ROW);
    }
    sqlite3_finalize(q);
    if (!has) rc = hppliteInternalExec(pCtx, db, "ALTER TABLE _hpplite_pending ADD COLUMN mode INTEGER NOT NULL DEFAULT 0");
  }
  /* files from before #57 (no batch was ever left open) and #56 (no
  ** statement recorded a time) */
  if (rc == SQLITE_OK) rc = addColumnIfMissing(pCtx, db, "_hpplite_meta", "open_txn");
  if (rc == SQLITE_OK) rc = addColumnIfMissing(pCtx, db, "_hpplite_pending", "now");
  if (rc == SQLITE_OK) rc = addColumnIfMissing(pCtx, db, "_hpplite_carry", "now");
  /* #59: statements posted in pieces */
  if (rc == SQLITE_OK) rc = addColumnIfMissing(pCtx, db, "_hpplite_meta", "frag_id");
  if (rc == SQLITE_OK) rc = addColumnIfMissing(pCtx, db, "_hpplite_meta", "frag_off");
  if (rc == SQLITE_OK) rc = addColumnIfMissing(pCtx, db, "_hpplite_carry", "frag");
  if (rc == SQLITE_OK) {
    /* the rollup the history is on */
    sqlite3_stmt *q = NULL;
    int has = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info('_hpplite_meta') WHERE name = 'rollup'", -1, &q, NULL) == SQLITE_OK)
      has = (sqlite3_step(q) == SQLITE_ROW);
    sqlite3_finalize(q);
    if (!has) rc = hppliteInternalExec(pCtx, db, "ALTER TABLE _hpplite_meta ADD COLUMN rollup TEXT NOT NULL DEFAULT ''");
  }
  if (rc != SQLITE_OK) {
    sqlite3_free(pCtx);
    return rc;
  }
  loadMeta(pCtx);
  pCtx->seenSchema = hppliteSchemaVersion(pCtx);
  pCtx->bClockPin = hpplite_clock_on(db);
  hpplitePragmaModeRead(pCtx);

  sqlite3_commit_hook(db, hppliteCommitHook, pCtx);
  sqlite3_rollback_hook(db, hppliteRollbackHook, pCtx);
  sqlite3_trace_v2(db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE, hppliteTraceCallback, pCtx);
  sqlite3_set_authorizer(db, hppliteAuthorizer, pCtx);

  *ppCtx = pCtx;
  return SQLITE_OK;
}

static void rearmHooks(HppliteCtx *pCtx, int bClosing) {
  void *prevCommit, *prevRollback;
  int lost = 0;
  if (!pCtx || pCtx->bDbClosed) return;
  /* setting a hook returns the previous argument: not ours means replaced */
  prevCommit = sqlite3_commit_hook(pCtx->db, hppliteCommitHook, pCtx);
  prevRollback = sqlite3_rollback_hook(pCtx->db, hppliteRollbackHook, pCtx);
  if (prevCommit != pCtx || prevRollback != pCtx) lost = 1;
  /* A driver clearing the connection's callbacks as it closes it (Python's
  ** sqlite3 does) is not another component taking them over: nothing runs
  ** after it */
  if (bClosing && (prevCommit == pCtx || !prevCommit) && (prevRollback == pCtx || !prevRollback)) lost = 0;
  /* the trace API returns no previous value: re-arm it unconditionally and
  ** rely on the commit hook as the canary (a library that replaces one
  ** usually replaces the others; the data_version check in #37 covers the rest) */
  sqlite3_trace_v2(pCtx->db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE, hppliteTraceCallback, pCtx);
  sqlite3_set_authorizer(pCtx->db, hppliteAuthorizer, pCtx);
  if (lost) {
    pCtx->nHooksReplaced++;
    if (pCtx->nHooksReplaced == 1) {
      fprintf(stderr, "hpplite: another component replaced the connection's commit/trace hooks; "
                      "writes made meanwhile were not captured (re-armed, hooks_replaced counts this)\n");
    }
  }
}
void hpplite_rearm_hooks(HppliteCtx *pCtx) { rearmHooks(pCtx, 0); }

/*
** Finalize the statements HPPLite keeps prepared on the application
** connection. They are re-prepared on demand, so this is safe at any time;
** it is needed before a close, which fails while any statement is live.
*/
static void finalizeOwnStatements(HppliteCtx *pCtx) {
  if (pCtx->pLogStmt) sqlite3_finalize(pCtx->pLogStmt);
  if (pCtx->pUnlogStmt) sqlite3_finalize(pCtx->pUnlogStmt);
  if (pCtx->pRecTrigStmt) sqlite3_finalize(pCtx->pRecTrigStmt);
  if (pCtx->pSchemaStmt) sqlite3_finalize(pCtx->pSchemaStmt);
  if (pCtx->pZeroStmt) sqlite3_finalize(pCtx->pZeroStmt);
  if (pCtx->pSeqStmt) sqlite3_finalize(pCtx->pSeqStmt);
  if (pCtx->pRowsSchemaStmt) sqlite3_finalize(pCtx->pRowsSchemaStmt);
  if (pCtx->pRowsTempSchemaStmt) sqlite3_finalize(pCtx->pRowsTempSchemaStmt);
  if (pCtx->pRowsLockStmt) sqlite3_finalize(pCtx->pRowsLockStmt);
  pCtx->pLogStmt = pCtx->pUnlogStmt = pCtx->pSchemaStmt = pCtx->pRecTrigStmt = pCtx->pZeroStmt = NULL;
  pCtx->pSeqStmt = pCtx->pRowsSchemaStmt = pCtx->pRowsTempSchemaStmt = pCtx->pRowsLockStmt = NULL;
  pCtx->pLastLogged = NULL;
}

/* Statements of the application that are still open (ours do not count).
** Application thread only: it walks the connection. */
static int applicationStatementsOpen(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt = NULL;
  while ((pStmt = sqlite3_next_stmt(pCtx->db, pStmt)) != NULL) {
    if (pStmt == pCtx->pLogStmt || pStmt == pCtx->pUnlogStmt || pStmt == pCtx->pSchemaStmt
        || pStmt == pCtx->pRecTrigStmt || pStmt == pCtx->pZeroStmt || pStmt == pCtx->pSeqStmt
        || pStmt == pCtx->pRowsSchemaStmt || pStmt == pCtx->pRowsTempSchemaStmt || pStmt == pCtx->pRowsLockStmt) continue;
    return 1;
  }
  return 0;
}

void hpplite_shutdown(HppliteCtx *pCtx) {
  if (!pCtx) return;

  if (!pCtx->bDbClosed) {
    sqlite3_commit_hook(pCtx->db, NULL, NULL);
    sqlite3_rollback_hook(pCtx->db, NULL, NULL);
    sqlite3_trace_v2(pCtx->db, 0, NULL, NULL);
    sqlite3_set_authorizer(pCtx->db, NULL, NULL);
    hpplite_rows_disable(pCtx);           /* its triggers and functions hold pCtx (#66) */
    finalizeOwnStatements(pCtx);
  }
  hppliteFreeFunctionChecks(pCtx);
  sqlite3_free(pCtx->aNoClock);
  sqlite3_free(pCtx->zRowsTextOnly);
  sqlite3_free(pCtx->zRowsSeq);
  sqlite3_free(pCtx->zDbName);
  if (pCtx->pNoClockMutex) sqlite3_mutex_free(pCtx->pNoClockMutex);
  sqlite3_free(pCtx);
}

int hpplite_set_bookkeeping_db(HppliteCtx *pCtx, sqlite3 *bkDb) {
  if (!pCtx) return SQLITE_MISUSE;
  pCtx->bkDb = bkDb ? bkDb : pCtx->db;
  return loadMeta(pCtx);
}

void hpplite_enable(HppliteCtx *pCtx, int bEnable) {
  if (pCtx) pCtx->bEnabled = bEnable;
}

void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut) {
  if (!pCtx || !pOut) return;
  if (pCtx->bRootDirty) {
    int saved = bkEnter(pCtx);
    if (hpplite_db_hash(pCtx->bkDb, pCtx->curRoot) == SQLITE_OK) {
      pCtx->bRootDirty = 0;
    }
    bkLeave(pCtx, saved);
  }
  memcpy(pOut, pCtx->curRoot, HPPLITE_HASH_SIZE);
}

void hpplite_set_callbacks(
  HppliteCtx *pCtx,
  void *pArg,
  void (*xOnCommit)(void*, unsigned char*, int),
  void (*xOnRollback)(void*)
) {
  if (pCtx) {
    pCtx->pCallbackArg = pArg;
    pCtx->xOnCommit = xOnCommit;
    pCtx->xOnRollback = xOnRollback;
  }
}

/*
** ============================================================================
** Block Builder Implementation
** ============================================================================
*/

int hpplite_exec(HppliteCtx *pCtx, const char *zSql, char **pzErrMsg) {
  return hpplite_exec_cb(pCtx, zSql, NULL, NULL, pzErrMsg);
}

int hpplite_exec_cb(
  HppliteCtx *pCtx,
  const char *zSql,
  int (*xCallback)(void*, int, char**, char**),
  void *pArg,
  char **pzErrMsg
) {
  if (!pCtx || !zSql) return SQLITE_MISUSE;
  /* The trace callback logs the statement; nothing else to do here */
  return sqlite3_exec(pCtx->db, zSql, xCallback, pArg, pzErrMsg);
}

/*
** Bookkeeping transactions. With a dedicated bookkeeping connection these
** never touch the application's transaction. On the shared (:memory:)
** connection they are only possible while the application is in autocommit.
*/
static int bkBegin(HppliteCtx *pCtx, int bWrite) {
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
  ** flush. */
  if (bWrite && pCtx->bkDb != pCtx->db && !pCtx->bAppFlushing
      && pCtx->nAppStmtActive > 0) {
    return SQLITE_BUSY;
  }
  /* The same between statements of an application transaction: a read
  ** snapshot taken before our commit cannot become a write after it, so the
  ** transaction's next write would fail with "database is locked" (Knex's
  ** deferred BEGIN, Django's atomic(): read, then write). Even the
  ** application's own hpplite_flush() waits for its COMMIT. */
  if (bWrite && pCtx->bkDb != pCtx->db && pCtx->bAppTxnOpen) {
    return SQLITE_BUSY;
  }
  if (bWrite && pCtx->bkDb != pCtx->db && !pCtx->bAppFlushing) {
    int expected = 0;
    if (!__atomic_compare_exchange_n(&pCtx->txnGate, &expected, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
      return SQLITE_BUSY;                 /* the application is between read and write (or the
                                          ** gate is held: its holder's flag is not ours to clear) */
    }
    pCtx->bBkGate = 1;
  } else {
    pCtx->bBkGate = 0;
  }
  pCtx->commitsDoneBefore = __atomic_load_n(&pCtx->nCommitDone, __ATOMIC_ACQUIRE);   /* before this look's snapshot (#37) */
  {
    int rc = hppliteInternalExec(pCtx, pCtx->bkDb, bWrite ? "BEGIN IMMEDIATE" : "BEGIN");
    if (rc != SQLITE_OK && pCtx->bBkGate) {
      pCtx->bBkGate = 0;
      __atomic_store_n(&pCtx->txnGate, 0, __ATOMIC_RELEASE);
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
  int i;
  for (i = 0; i < n; i++) h = (h ^ z[i]) * 1099511628211LL;
  return h;
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
#define TEST_FAIL_COMMIT(rc) do { if ((rc) == SQLITE_OK && hpplite_test_fail_commits > 0) { hpplite_test_fail_commits--; (rc) = SQLITE_FULL; } } while (0)
#else
#define TEST_FAIL_COMMIT(rc) ((void)0)
#endif
static int bkEnd(HppliteCtx *pCtx, int rc) {
  int bLook = rc == SQLITE_OK && pCtx->bkDb != pCtx->db;
  if (bLook) bkForeignCheck(pCtx, pCtx->commitsDoneBefore);
  if (rc == SQLITE_OK) {
    rc = hppliteInternalExec(pCtx, pCtx->bkDb, "COMMIT");
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
  }
  if (pCtx->bBkGate) {
    pCtx->bBkGate = 0;
    __atomic_store_n(&pCtx->txnGate, 0, __ATOMIC_RELEASE);
  }
  return rc;
}

struct HppliteBatch *hpplite_build_batch(HppliteCtx *pCtx, sqlite3_int64 *pMaxId) {
  return hpplite_build_batch_limited(pCtx, 0, 1, pMaxId);
}

/*
** Drop the last statement of a batch.
*/
static void batchPop(HppliteBatch *pBatch) {
  if (pBatch->nTxns > 0) {
    pBatch->nTxns--;
    sqlite3_free(pBatch->aTxns[pBatch->nTxns].zSql);
    pBatch->aTxns[pBatch->nTxns].zSql = NULL;
  }
}

static int cmpNoClock(const void *a, const void *b) {
  const struct HppliteNoClock *x = a, *y = b;
  if (x->id != y->id) return x->id < y->id ? -1 : 1;
  if (x->now != y->now) return x->now < y->now ? -1 : 1;
  return 0;
}

/* Drop the trailing statements of the last group, unless the group began
** the batch (then it is larger than a batch and is cut where it is) */
static void cutToGroupStart(HppliteBatch *pBatch) {
  int64_t cutTxn = pBatch->aTxns[pBatch->nTxns - 1].txn;
  int i = pBatch->nTxns;
  while (i > 0 && pBatch->aTxns[i - 1].txn == cutTxn) i--;
  if (i > 0) {
    while (pBatch->nTxns > i) batchPop(pBatch);
  }
}

/*
** sqlite_sequence entries no record carries (#19): a write that changed no
** row can create one (an INSERT ... SELECT of nothing into a new
** AUTOINCREMENT table). Before each batch the flusher logs what moved in
** sqlite_sequence since it last looked, as sequence records - from the
** bookkeeping connection, so the application's changes() stays its own, and
** for every connection writing the file. Set exactly, at that point of the
** log: what any committed transaction did, by then.
*/
int hpplite_rows_seq_flush(HppliteCtx *pCtx) {
  char *zMoved, *zNow = NULL;
  int n = 0, rc, saved;
  sqlite3_stmt *q = NULL;
  const char *z;
  if (!pCtx || !pCtx->bRows || pCtx->bkDb == pCtx->db) return SQLITE_OK;
  if (bkBegin(pCtx, 1) != SQLITE_OK) return SQLITE_BUSY;
  saved = bkEnter(pCtx);
  zMoved = hpplite_rows_seq_changed(pCtx, pCtx->bkDb, &n, &zNow);
  rc = zMoved ? SQLITE_OK : SQLITE_NOMEM;
  if (rc == SQLITE_OK && n > 0) {
    rc = sqlite3_prepare_v2(pCtx->bkDb,
      "INSERT INTO _hpplite_pending(txn, sql, mode, now, tbl, op, rec) VALUES(0, '', ?1, 0, ?2, ?3, ?4)", -1, &q, NULL);
    for (z = zMoved; rc == SQLITE_OK && *z; ) {
      const char *v = z + strlen(z) + 1;
      int nRec = 0;
      unsigned char *aRec = hpplite_rows_seq_rec(atoll(v), &nRec);
      if (!aRec) { rc = SQLITE_NOMEM; break; }
      sqlite3_bind_int(q, 1, HPPLITE_MODE_ROW);
      sqlite3_bind_text(q, 2, z, -1, SQLITE_STATIC);
      sqlite3_bind_int(q, 3, HPPLITE_ROW_SEQ);
      sqlite3_bind_blob(q, 4, aRec, nRec, sqlite3_free);
      rc = sqlite3_step(q) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
      sqlite3_reset(q);
      z = v + strlen(v) + 1;
    }
    sqlite3_finalize(q);
  }
  sqlite3_free(zMoved);
  bkLeave(pCtx, saved);
  rc = bkEnd(pCtx, rc);
  /* the snapshot is what the records were made from, read in the
  ** transaction - not read again after it, when another connection's
  ** commit in between would be taken in unlogged (#91) */
  if (rc == SQLITE_OK) hpplite_rows_seq_install(pCtx, zNow); else sqlite3_free(zNow);
  return rc;
}

struct HppliteBatch *hpplite_build_batch_limited(HppliteCtx *pCtx, size_t maxBytes,
                                                 int withRoot, sqlite3_int64 *pMaxId) {
  HppliteBatch *pBatch = NULL;
  sqlite3_stmt *pStmt = NULL;
  sqlite3_int64 *aId = NULL, *aGroup = NULL;   /* per row read: log id, absolute group */
  sqlite3_int64 maxId = 0, lastTxn = -1, txnRel = -1, breakGroup = 0;
  int rc, nAlloc = 0, nRead = 0, haveBreak = 0, split = 0;
  size_t splitStart = 0;                        /* where in its row's text the last entry starts */

  if (!pCtx) return NULL;
  if (bkBegin(pCtx, 0) != SQLITE_OK) return NULL;
  int savedInternal = bkEnter(pCtx);

  /* Read pending rows in order. With a size limit, stop as soon as the
  ** estimated JSON size passes it: the rest belongs to later batches, and
  ** a large backlog must not be re-read for every chunk. */
  size_t estSize = hpplite_batch_json_overhead();
  int overLimit = 0;
  /* a log some connection has row capture on for has records in it (#19) */
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, CASE WHEN txn = 0 THEN id ELSE txn END, sql, mode, now, op, tbl, rec FROM _hpplite_pending ORDER BY id",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, CASE WHEN txn = 0 THEN id ELSE txn END, sql, mode, now FROM _hpplite_pending ORDER BY id",
    -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    char *zEntry = NULL;
    while (sqlite3_step(pStmt) == SQLITE_ROW) {
      sqlite3_int64 id = sqlite3_column_int64(pStmt, 0);
      sqlite3_int64 txn = sqlite3_column_int64(pStmt, 1);
      const char *zSql = (const char*)sqlite3_column_text(pStmt, 2);
      const char *zText = zSql ? zSql : "";
      sqlite3_free(zEntry);
      zEntry = NULL;
      if (sqlite3_column_count(pStmt) > 5 && sqlite3_column_int(pStmt, 5) > 0) {
        zEntry = hpplite_rows_entry(sqlite3_column_int(pStmt, 5), (const char*)sqlite3_column_text(pStmt, 6),
                                    (const unsigned char*)sqlite3_column_blob(pStmt, 7), sqlite3_column_bytes(pStmt, 7));
        if (!zEntry) break;
        zText = zEntry;
      }
      size_t off = 0, pieceLen = 0;
      int piece = 0;
      if (!pBatch) {
        pBatch = hpplite_batch_new(pCtx->blockHeight);
        if (!pBatch) break;
        /* a named database's first batch says its name (#61) */
        if (pCtx->zDbName && pBatch->height == 1) pBatch->zDbName = sqlite3_mprintf("%s", pCtx->zDbName);
      }
      /* the statement the last batch posted part of continues where it stopped (#59) */
      if (pCtx->fragId && id == pCtx->fragId && pCtx->fragOff <= strlen(zText)) {
        off = pCtx->fragOff;
        zText += off;
      }
      if (maxBytes > 0) {
        size_t sz = hpplite_batch_stmt_json_size(zText, -1, txnRel) + HPPLITE_FRAG_FLAG_BYTES;
        if (estSize + sz > maxBytes) {
          /* One that could not fit even an empty batch is cut into pieces:
          ** this batch carries what fits, the next ones the rest (#59).
          ** One that fits a batch of its own starts the next batch. */
          size_t alone = maxBytes > hpplite_batch_json_overhead() ? maxBytes - hpplite_batch_json_overhead() : 0;
          size_t room = maxBytes > estSize + HPPLITE_FRAG_ENTRY_BYTES ? maxBytes - estSize - HPPLITE_FRAG_ENTRY_BYTES : 0;
          if (sz > alone && room >= HPPLITE_FRAG_MIN) {
            pieceLen = hpplite_json_prefix_fit(zText, room);
            if (pieceLen > 0 && zText[pieceLen]) piece = 1;
          }
          if (!piece && pBatch->nTxns > 0) {
            overLimit = 1;          /* this row starts the next batch */
            breakGroup = txn;
            haveBreak = 1;
            break;
          }
        }
        estSize += sz;
      }
      if (pBatch->nTxns >= nAlloc) {
        sqlite3_int64 *aNew, *aNewG;
        nAlloc = nAlloc ? nAlloc * 2 : 64;
        aNew = sqlite3_realloc(aId, (int)(sizeof(sqlite3_int64) * nAlloc));
        if (!aNew) break;
        aId = aNew;
        aNewG = sqlite3_realloc(aGroup, (int)(sizeof(sqlite3_int64) * nAlloc));
        if (!aNewG) break;
        aGroup = aNewG;
      }
      if (txn != lastTxn) { txnRel++; lastTxn = txn; }
      if (piece) {
        char *zPiece = sqlite3_mprintf("%.*s", (int)pieceLen, zText);
        hpplite_batch_add_stmt(pBatch, zPiece ? zPiece : "", -1, txnRel);
        sqlite3_free(zPiece);
        pBatch->aTxns[pBatch->nTxns - 1].frag = 1;
      } else {
        hpplite_batch_add_stmt(pBatch, zText, -1, txnRel);
      }
      pBatch->aTxns[pBatch->nTxns - 1].mode = sqlite3_column_int(pStmt, 3);
      pBatch->aTxns[pBatch->nTxns - 1].now = sqlite3_column_int64(pStmt, 4);
      aId[pBatch->nTxns - 1] = id;
      aGroup[pBatch->nTxns - 1] = txn;
      nRead = pBatch->nTxns;
      /* the first group continues the one the last posted batch left open */
      if (pBatch->nTxns == 1 && pCtx->openTxnId != 0 && txn == pCtx->openTxnId) pBatch->contTxn = 1;
      if (piece) {
        split = 1;
        splitStart = off;
        pBatch->fragRow = id;
        pBatch->fragOff = off + pieceLen;
        break;                      /* the rest of it starts the next batch */
      }
    }
    sqlite3_free(zEntry);
    sqlite3_finalize(pStmt);
  }
  if (pBatch && split) pBatch->partial = 1;
  if (pBatch && overLimit) {
    /* The size estimate stopped the read inside a transaction group: move
    ** the group whole to the next batch when it began after other groups
    ** (it may fit there); one that fills a batch from its start is larger
    ** than a batch and continues in the next one (#57) */
    if (breakGroup == aGroup[pBatch->nTxns - 1]) cutToGroupStart(pBatch);
    pBatch->partial = 1;
  }

  if (pBatch) {
    pBatch->timestamp = (uint64_t)time(NULL);
    /* State root is taken in the same snapshot as the pending rows. A cut
    ** batch carries no root (the series' last one does), so do not pay for
    ** the O(database) hash on chunks that will drop it anyway. */
    if (withRoot && !pBatch->partial) {
      unsigned char root[HPPLITE_HASH_SIZE];
      if (hpplite_db_hash(pCtx->bkDb, root) == SQLITE_OK) {
        hpplite_batch_set_post_state(pBatch, root);
      } else {
        pCtx->nLogErrors++;
        hpplite_error(pCtx, "cannot hash the database for the state root (%s); batch %llu not built",
                sqlite3_errmsg(pCtx->bkDb), (unsigned long long)pBatch->height);
        hpplite_batch_free(pBatch);
        pBatch = NULL;
      }
    }
  }
  bkLeave(pCtx, savedInternal);
  bkEnd(pCtx, SQLITE_OK);

  /* The estimate is conservative; confirm with one real serialization and
  ** drop trailing statements in the rare case it still does not fit. Then
  ** back up to a transaction boundary when that leaves anything. */
  if (pBatch && maxBytes > 0) {
    int nFull = pBatch->nTxns;
    for (;;) {
      char *zJson = hpplite_batch_to_json(pBatch);
      size_t n = zJson ? strlen(zJson) : 0;
      sqlite3_free(zJson);
      if (n <= maxBytes) break;
      if (pBatch->aTxns[pBatch->nTxns - 1].frag) {
        /* the estimate was short: cut the piece shorter, never drop it */
        HppliteTxn *t = &pBatch->aTxns[pBatch->nTxns - 1];
        size_t have = strlen(t->zSql), over = n - maxBytes + 64;
        size_t keep = hpplite_json_prefix_fit(t->zSql, have > over ? have - over : 0);
        if (keep == 0) { hpplite_batch_free(pBatch); pBatch = NULL; break; }
        t->zSql[keep] = 0;
        pBatch->fragOff = splitStart + keep;
        continue;
      }
      if (pBatch->nTxns <= 1) {
        pCtx->nLogErrors++;
        hpplite_error(pCtx, "statement of %zu bytes exceeds the batch size limit (%zu); left pending",
                n, maxBytes);
        hpplite_batch_free(pBatch);
        pBatch = NULL;
        break;
      }
      batchPop(pBatch);
      pBatch->partial = 1;
    }
    if (pBatch && pBatch->partial) {
      /* The root describes the content after ALL pending SQL; a cut batch
      ** does not end there, so it carries none - the last of the series does */
      pBatch->hasRoot = 0;
      memset(pBatch->postStateRoot, 0, HPPLITE_HASH_SIZE);
    }
    if (pBatch && pBatch->partial && pBatch->nTxns < nFull && !split
        && aGroup[pBatch->nTxns] == aGroup[pBatch->nTxns - 1]) {
      /* the serialization cut landed inside a group: same rule */
      cutToGroupStart(pBatch);
    }
  }

  if (pBatch && split) {
    /* ends in a piece: its statement, so its group, continues */
    pBatch->openTxn = 1;
    pBatch->openGroup = aGroup[pBatch->nTxns - 1];
  } else if (pBatch && pBatch->partial) {
    /* Open: the first row left out belongs to the last group kept */
    sqlite3_int64 next = 0;
    int haveNext = 0;
    if (pBatch->nTxns < nRead) { next = aGroup[pBatch->nTxns]; haveNext = 1; }
    else if (haveBreak) { next = breakGroup; haveNext = 1; }
    if (haveNext && next == aGroup[pBatch->nTxns - 1]) {
      pBatch->openTxn = 1;
      pBatch->openGroup = next;
    }
  }
  if (pBatch && pCtx->nNoClock > 0 && pCtx->pNoClockMutex) {
    /* statements known not to have read the clock: their time stays out */
    struct HppliteNoClock *a = NULL;
    int n = 0, i;
    sqlite3_mutex_enter(pCtx->pNoClockMutex);
    if (pCtx->nNoClock > 0) {
      a = sqlite3_malloc64(sizeof(*a) * (size_t)pCtx->nNoClock);
      if (a) { memcpy(a, pCtx->aNoClock, sizeof(*a) * (size_t)pCtx->nNoClock); n = pCtx->nNoClock; }
    }
    sqlite3_mutex_leave(pCtx->pNoClockMutex);
    if (a) {
      qsort(a, (size_t)n, sizeof(*a), cmpNoClock);
      for (i = 0; i < pBatch->nTxns; i++) {
        struct HppliteNoClock key, *hit;
        if (!pBatch->aTxns[i].now) continue;
        key.id = aId[i]; key.now = pBatch->aTxns[i].now;
        hit = bsearch(&key, a, (size_t)n, sizeof(*a), cmpNoClock);
        if (hit) pBatch->aTxns[i].now = 0;
      }
      sqlite3_free(a);
    }
  }
  /* the rows posted whole; a piece's row stays pending until its last piece */
  if (pBatch) maxId = split ? (pBatch->nTxns >= 2 ? aId[pBatch->nTxns - 2] : 0) : aId[pBatch->nTxns - 1];
  sqlite3_free(aId);
  sqlite3_free(aGroup);
  if (pMaxId) *pMaxId = maxId;
  return pBatch;
}

sqlite3_int64 hpplite_pending_bytes(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  sqlite3_int64 n = 0;
  if (!pCtx) return 0;
  int saved = bkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT COALESCE(SUM(length(sql)), 0) FROM _hpplite_pending",
                         -1, &pStmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) n = sqlite3_column_int64(pStmt, 0);
    sqlite3_finalize(pStmt);
  }
  bkLeave(pCtx, saved);
  return n;
}

/*
** Advance _hpplite_meta past pBatch and optionally delete pending rows.
** Caller holds a bookkeeping write transaction.
*/
static int advanceMeta(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId, int bWriter) {
  unsigned char blockHash[HPPLITE_HASH_SIZE];
  sqlite3_stmt *pStmt;
  int rc;

  hpplite_block_hash(pBatch, pCtx->prevBlockHash, blockHash);

  if (maxId > 0) {
    rc = sqlite3_prepare_v2(pCtx->bkDb,
      "DELETE FROM _hpplite_pending WHERE id <= ?1", -1, &pStmt, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int64(pStmt, 1, maxId);
    rc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    if (rc != SQLITE_DONE) return rc;
  }

  if (bWriter && pBatch->pPosted && pBatch->nPosted) {
    /* keep the last HPPLITE_POSTED_KEEP batches as posted */
    rc = sqlite3_prepare_v2(pCtx->bkDb, "INSERT OR REPLACE INTO _hpplite_posted(height, data) VALUES(?1, ?2)",
                            -1, &pStmt, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)pBatch->height);
    sqlite3_bind_blob(pStmt, 2, pBatch->pPosted, (int)pBatch->nPosted, SQLITE_STATIC);
    rc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    if (rc != SQLITE_DONE) return rc;
    rc = sqlite3_prepare_v2(pCtx->bkDb, "DELETE FROM _hpplite_posted WHERE height <= ?1", -1, &pStmt, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)pBatch->height - HPPLITE_POSTED_KEEP);
    rc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    if (rc != SQLITE_DONE) return rc;
  }
  if (maxId > 0 && pCtx->nNoClock > 0 && pCtx->pNoClockMutex) {
    int i, k = 0;
    sqlite3_mutex_enter(pCtx->pNoClockMutex);
    for (i = 0; i < pCtx->nNoClock; i++) if (pCtx->aNoClock[i].id > maxId) pCtx->aNoClock[k++] = pCtx->aNoClock[i];
    pCtx->nNoClock = k;
    sqlite3_mutex_leave(pCtx->pNoClockMutex);
  }
  if (bWriter && pCtx->nCarry > 0) {
    /* we post now: a part held from another writer's open transaction will
    ** never be continued (its replicas drop it on this batch too) */
    rc = sqlite3_exec(pCtx->bkDb, "DELETE FROM _hpplite_carry", NULL, NULL, NULL);
    if (rc != SQLITE_OK) return rc;
    pCtx->nTxnsDropped++;
    pCtx->nCarry = 0;
  }

  rc = sqlite3_prepare_v2(pCtx->bkDb,
    pBatch->hasRoot
      ? "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, open_txn=?4, frag_id=?5, frag_off=?6,"
        " state_root=?3 WHERE id=1"
      : "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, open_txn=?4, frag_id=?5, frag_off=?6 WHERE id=1",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)(pBatch->height + 1));
  sqlite3_bind_blob(pStmt, 2, blockHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
  if (pBatch->hasRoot) {
    sqlite3_bind_blob(pStmt, 3, pBatch->postStateRoot, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
  }
  /* writer: the group this batch leaves open, continued by the next one */
  sqlite3_bind_int64(pStmt, 4, (bWriter && pBatch->openTxn) ? pBatch->openGroup : 0);
  /* writer: the statement this batch ends a piece of, and how far it is posted */
  sqlite3_bind_int64(pStmt, 5, bWriter ? pBatch->fragRow : 0);
  sqlite3_bind_int64(pStmt, 6, bWriter ? (sqlite3_int64)pBatch->fragOff : 0);
  rc = sqlite3_step(pStmt);
  sqlite3_finalize(pStmt);
  if (rc != SQLITE_DONE) return rc;

  pCtx->blockHeight = pBatch->height + 1;
  pCtx->openTxnId = (bWriter && pBatch->openTxn) ? pBatch->openGroup : 0;
  pCtx->fragId = bWriter ? pBatch->fragRow : 0;
  pCtx->fragOff = bWriter ? pBatch->fragOff : 0;
  memcpy(pCtx->prevBlockHash, blockHash, HPPLITE_HASH_SIZE);
  if (pBatch->hasRoot) {
    memcpy(pCtx->stateRoot, pBatch->postStateRoot, HPPLITE_HASH_SIZE);
  }
  return SQLITE_OK;
}

int hpplite_bind_rollup(HppliteCtx *pCtx, const char *zRollup) {
  sqlite3_stmt *pStmt;
  int rc;
  if (!pCtx || !zRollup || !zRollup[0]) return 0;
  if (pCtx->zRollup[0]) return strcmp(pCtx->zRollup, zRollup) == 0 ? 0 : -1;
  rc = bkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return -2;
  {
    int saved = bkEnter(pCtx);
    rc = sqlite3_prepare_v2(pCtx->bkDb, "UPDATE _hpplite_meta SET rollup = ?1 WHERE id = 1", -1, &pStmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_text(pStmt, 1, zRollup, -1, SQLITE_STATIC);
      rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
      sqlite3_finalize(pStmt);
    }
    bkLeave(pCtx, saved);
  }
  rc = bkEnd(pCtx, rc);
  if (rc != SQLITE_OK) return -2;
  snprintf(pCtx->zRollup, sizeof(pCtx->zRollup), "%s", zRollup);
  return 1;
}

uint8_t *hpplite_posted_batch(HppliteCtx *pCtx, uint64_t height, size_t *pLen) {
  sqlite3_stmt *pStmt;
  uint8_t *out = NULL;
  int saved;
  *pLen = 0;
  if (!pCtx) return NULL;
  saved = bkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT data FROM _hpplite_posted WHERE height = ?1", -1, &pStmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height);
    if (sqlite3_step(pStmt) == SQLITE_ROW) {
      int n = sqlite3_column_bytes(pStmt, 0);
      out = malloc(n > 0 ? (size_t)n : 1);
      if (out) { memcpy(out, sqlite3_column_blob(pStmt, 0), (size_t)n); *pLen = (size_t)n; }
    }
    sqlite3_finalize(pStmt);
  }
  bkLeave(pCtx, saved);
  return out;
}

int hpplite_finalize_batch(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId) {
  int rc;
  if (!pCtx || !pBatch) return SQLITE_MISUSE;
  if (pBatch->height != pCtx->blockHeight) return SQLITE_MISMATCH;
  rc = bkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  {
    int saved = bkEnter(pCtx);
    rc = advanceMeta(pCtx, pBatch, maxId, 1);
    bkLeave(pCtx, saved);
  }
  TEST_FAIL_COMMIT(rc);
  rc = bkEnd(pCtx, rc);
  /* a commit that failed (a full disk) rolled the file back: the height and
  ** the rest of the meta row that advanceMeta() set must follow, or the next
  ** flush posts the same rows again at the next height (#97) */
  if (rc != SQLITE_OK) loadMeta(pCtx);
  return rc;
}

struct HppliteBatch *hpplite_flush_block(HppliteCtx *pCtx) {
  sqlite3_int64 maxId = 0;
  HppliteBatch *pBatch = hpplite_build_batch(pCtx, &maxId);
  if (!pBatch) return NULL;
  if (hpplite_finalize_batch(pCtx, pBatch, maxId) != SQLITE_OK) {
    hpplite_batch_free(pBatch);
    return NULL;
  }
  return pBatch;
}

/*
** Put the replay connection in the state a statement ran under (#52).
** Foreign keys can only change outside a transaction - which is also why
** every statement of one writer transaction shares the setting.
*/
static int replayMode(sqlite3 *rdb, int mode, int *pCur) {
  int want, rc = SQLITE_OK;
  if (mode == *pCur) return SQLITE_OK;
  want = (mode & HPPLITE_MODE_FOREIGN_KEYS) != 0;
  if (want != ((*pCur & HPPLITE_MODE_FOREIGN_KEYS) != 0) && sqlite3_get_autocommit(rdb)) {
    int out = -1;
    sqlite3_db_config(rdb, SQLITE_DBCONFIG_ENABLE_FKEY, want, &out);
    if (out != want) return SQLITE_ERROR;
    *pCur = (*pCur & ~HPPLITE_MODE_FOREIGN_KEYS) | (want ? HPPLITE_MODE_FOREIGN_KEYS : 0);
  }
  want = (mode & HPPLITE_MODE_RECURSIVE_TRIGGERS) != 0;
  if (want != ((*pCur & HPPLITE_MODE_RECURSIVE_TRIGGERS) != 0)) {
    rc = sqlite3_exec(rdb, want ? "PRAGMA recursive_triggers = 1" : "PRAGMA recursive_triggers = 0", NULL, NULL, NULL);
    if (rc == SQLITE_OK) *pCur = (*pCur & ~HPPLITE_MODE_RECURSIVE_TRIGGERS) | (want ? HPPLITE_MODE_RECURSIVE_TRIGGERS : 0);
  }
  /* the connection pragmas (#28); writable_schema goes off with RESET, which
  ** also reloads the schema the statements under it may have edited */
  {
    static const struct { int bit; const char *zOn, *zOff; } aPragma[] = {
      { HPPLITE_MODE_LEGACY_ALTER,    "PRAGMA legacy_alter_table = 1",       "PRAGMA legacy_alter_table = 0" },
      { HPPLITE_MODE_WRITABLE_SCHEMA, "PRAGMA writable_schema = 1",          "PRAGMA writable_schema = RESET" },
      { HPPLITE_MODE_IGNORE_CHECKS,   "PRAGMA ignore_check_constraints = 1", "PRAGMA ignore_check_constraints = 0" },
      { HPPLITE_MODE_CASE_LIKE,       "PRAGMA case_sensitive_like = 1",      "PRAGMA case_sensitive_like = 0" },
    };
    size_t i;
    for (i = 0; rc == SQLITE_OK && i < sizeof(aPragma) / sizeof(aPragma[0]); i++) {
      want = (mode & aPragma[i].bit) != 0;
      if (want == ((*pCur & aPragma[i].bit) != 0)) continue;
      rc = sqlite3_exec(rdb, want ? aPragma[i].zOn : aPragma[i].zOff, NULL, NULL, NULL);
      if (rc == SQLITE_OK) *pCur = (*pCur & ~aPragma[i].bit) | (want ? aPragma[i].bit : 0);
    }
  }
  return rc;
}

static int connectionMode(sqlite3 *db) {
  int mode = 0, fk = 0;
  sqlite3_stmt *q = NULL;
  if (sqlite3_db_config(db, SQLITE_DBCONFIG_ENABLE_FKEY, -1, &fk) == SQLITE_OK && fk) mode |= HPPLITE_MODE_FOREIGN_KEYS;
  if (sqlite3_prepare_v2(db, "PRAGMA recursive_triggers", -1, &q, NULL) == SQLITE_OK
      && sqlite3_step(q) == SQLITE_ROW && sqlite3_column_int(q, 0)) {
    mode |= HPPLITE_MODE_RECURSIVE_TRIGGERS;
  }
  sqlite3_finalize(q);
  return mode;
}

/*
** A transaction larger than one batch (#57). The batch it starts in is
** open; the batches it continues in say cont. Replay applies complete
** groups, holds an open last group back in _hpplite_carry (written with the
** height, so it survives a restart) and runs it with the next batch's first
** group when that one says cont. A held part whose continuation never
** comes, or a continuation with nothing held, is dropped whole: the
** transaction is lost, never half of it - as if the writer had crashed
** before posting it.
*/
typedef struct ReplayStmt { const char *zSql; int mode; sqlite3_int64 group; sqlite3_int64 now; int frag; } ReplayStmt;

static int loadCarry(HppliteCtx *pCtx, char ***pazSql, int **paMode, sqlite3_int64 **paNow, int **paFrag) {
  sqlite3_stmt *q = NULL;
  int n = 0, cap = 0, rc;
  char **az = NULL;
  int *am = NULL, *af = NULL;
  sqlite3_int64 *an = NULL;
  *pazSql = NULL; *paMode = NULL; *paNow = NULL; *paFrag = NULL;
  if (pCtx->nCarry <= 0) return 0;
  rc = sqlite3_prepare_v2(pCtx->bkDb, "SELECT sql, mode, now, frag FROM _hpplite_carry ORDER BY seq", -1, &q, NULL);
  if (rc != SQLITE_OK) return -1;
  while (sqlite3_step(q) == SQLITE_ROW) {
    if (n == cap) {
      char **az2; int *am2, *af2; sqlite3_int64 *an2;
      cap = cap ? cap * 2 : 64;
      az2 = sqlite3_realloc64(az, sizeof(char*) * cap);
      if (!az2) { n = -1; break; }
      az = az2;
      am2 = sqlite3_realloc64(am, sizeof(int) * cap);
      if (!am2) { n = -1; break; }
      am = am2;
      an2 = sqlite3_realloc64(an, sizeof(sqlite3_int64) * cap);
      if (!an2) { n = -1; break; }
      an = an2;
      af2 = sqlite3_realloc64(af, sizeof(int) * cap);
      if (!af2) { n = -1; break; }
      af = af2;
    }
    az[n] = sqlite3_mprintf("%s", (const char*)sqlite3_column_text(q, 0));
    am[n] = sqlite3_column_int(q, 1);
    an[n] = sqlite3_column_int64(q, 2);
    af[n] = sqlite3_column_int(q, 3);
    n++;
  }
  sqlite3_finalize(q);
  *pazSql = az; *paMode = am; *paNow = an; *paFrag = af;
  return n;
}

static int storeCarry(HppliteCtx *pCtx, const ReplayStmt *a, int n) {
  sqlite3_stmt *q = NULL;
  int rc, i;
  rc = sqlite3_exec(pCtx->bkDb, "DELETE FROM _hpplite_carry", NULL, NULL, NULL);
  if (rc != SQLITE_OK || n == 0) return rc;
  rc = sqlite3_prepare_v2(pCtx->bkDb, "INSERT INTO _hpplite_carry(seq, sql, mode, now, frag) VALUES(?1, ?2, ?3, ?4, ?5)", -1, &q, NULL);
  for (i = 0; rc == SQLITE_OK && i < n; i++) {
    sqlite3_bind_int(q, 1, i);
    sqlite3_bind_text(q, 2, a[i].zSql, -1, SQLITE_STATIC);
    sqlite3_bind_int(q, 3, a[i].mode);
    sqlite3_bind_int64(q, 4, a[i].now);
    sqlite3_bind_int(q, 5, a[i].frag);
    rc = sqlite3_step(q) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_reset(q);
  }
  sqlite3_finalize(q);
  return rc;
}

/* The replica connection's own TEMP triggers (an application's, replayed as
** text): SQLITE_DBCONFIG_ENABLE_TRIGGER leaves TEMP triggers on, so they
** are set aside while records apply - their effects are records of their
** own - and put back for the text that fires them (#76). sqlite_schema
** stores a trigger's text without TEMP: put back as TEMP. */
static char *tempTriggersStash(sqlite3 *rdb) {
  sqlite3_stmt *q = NULL;
  char *zSql = NULL, *zDrop = NULL;
  if (sqlite3_prepare_v2(rdb, "SELECT name, sql FROM temp.sqlite_schema WHERE type = 'trigger' AND sql IS NOT NULL"
                         " AND name NOT LIKE 'hpplite\\_cap\\_%' ESCAPE '\\'", -1, &q, NULL) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) {
      const char *zName = (const char *)sqlite3_column_text(q, 0), *zCreate = (const char *)sqlite3_column_text(q, 1);
      if (sqlite3_strnicmp(zCreate, "CREATE TRIGGER", 14) == 0) zCreate += 14;
      else if (sqlite3_strnicmp(zCreate, "CREATE TEMP TRIGGER", 19) == 0) zCreate += 19;
      else if (sqlite3_strnicmp(zCreate, "CREATE TEMPORARY TRIGGER", 24) == 0) zCreate += 24;
      zSql = sqlite3_mprintf("%zCREATE TEMP TRIGGER%s;", zSql, zCreate);
      zDrop = sqlite3_mprintf("%zDROP TRIGGER temp.\"%w\";", zDrop, zName);
    }
  }
  sqlite3_finalize(q);
  if (zDrop) sqlite3_exec(rdb, zDrop, NULL, NULL, NULL);
  sqlite3_free(zDrop);
  return zSql;
}
static void tempTriggersRestore(sqlite3 *rdb, char **pzStash) {
  if (*pzStash) sqlite3_exec(rdb, *pzStash, NULL, NULL, NULL);
  sqlite3_free(*pzStash);
  *pzStash = NULL;
}

int hpplite_replay_batch(HppliteCtx *pCtx, const HppliteBatch *pBatch, int useTxn) {
  int savedEnabled, rc = SQLITE_OK, i, open = 0;
  sqlite3_int64 lastTxn = -1;
  unsigned char root[HPPLITE_HASH_SIZE];
  ReplayStmt *aRun = NULL;
  char **azCarry = NULL;
  int *aCarryMode = NULL;
  sqlite3_int64 *aCarryNow = NULL;
  int *aCarryFrag = NULL;
  char **azJoined = NULL;                       /* statements joined from pieces (#59) */
  int nJoined = 0;
  int nCarry, nRun = 0, nHold, firstEnd, holdFrom, carryChanged;
  sqlite3_int64 savedPin;
  unsigned savedSeen;

  if (!pCtx || !pBatch) return SQLITE_MISUSE;
  if (pBatch->height < pCtx->blockHeight) return SQLITE_DONE;   /* already applied */
  if (pBatch->height > pCtx->blockHeight) return SQLITE_MISMATCH; /* gap */

  /* Replay runs on the bookkeeping connection: no hooks there, so nothing
  ** is captured and the application thread's flags are left alone. */
  sqlite3 *rdb = pCtx->bkDb;
  /* A mode this version does not know cannot be reproduced: stop rather
  ** than replay into a different state */
  for (i = 0; i < pBatch->nTxns; i++) {
    if (pBatch->aTxns[i].mode & ~HPPLITE_MODE_KNOWN) {
      hpplite_error(pCtx, "batch %llu statement %d ran under connection state this version cannot apply (mode %d)",
              (unsigned long long)pBatch->height, i, pBatch->aTxns[i].mode);
      return SQLITE_MISMATCH;
    }
  }

  /* The statements to run, in order: what is held from the previous batch
  ** joins this batch's first group when it continues it */
  {
    int saved = bkEnter(pCtx);
    nCarry = loadCarry(pCtx, &azCarry, &aCarryMode, &aCarryNow, &aCarryFrag);
    bkLeave(pCtx, saved);
  }
  if (nCarry < 0) return SQLITE_NOMEM;
  aRun = sqlite3_malloc64(sizeof(ReplayStmt) * (size_t)(nCarry + pBatch->nTxns + 1));
  if (!aRun) {
    for (i = 0; i < nCarry; i++) sqlite3_free(azCarry[i]);
    sqlite3_free(azCarry); sqlite3_free(aCarryMode); sqlite3_free(aCarryNow); sqlite3_free(aCarryFrag);
    for (i = 0; i < nJoined; i++) sqlite3_free(azJoined[i]);
    sqlite3_free(azJoined);
    return SQLITE_NOMEM;
  }
  firstEnd = 0;
  while (firstEnd < pBatch->nTxns && pBatch->aTxns[firstEnd].txn == pBatch->aTxns[0].txn) firstEnd++;
  carryChanged = (nCarry > 0);
  if (nCarry > 0 && !pBatch->contTxn) {
    pCtx->nTxnsDropped++;
    hpplite_error(pCtx, "batch %llu does not continue the transaction batch %llu left open "
            "(%d statements held); dropped whole - its writer never posted the rest",
            (unsigned long long)pBatch->height, (unsigned long long)(pBatch->height - 1), nCarry);
  }
  if (nCarry > 0 && pBatch->contTxn) {
    for (i = 0; i < nCarry; i++) {
      aRun[nRun].zSql = azCarry[i]; aRun[nRun].mode = aCarryMode[i]; aRun[nRun].now = aCarryNow[i];
      aRun[nRun].frag = aCarryFrag[i];
      aRun[nRun].group = pBatch->nTxns > 0 ? pBatch->aTxns[0].txn : 0;
      nRun++;
    }
  }
  for (i = 0; i < pBatch->nTxns; i++) {
    if (i < firstEnd && pBatch->contTxn && nCarry == 0) continue;   /* a continuation without its head */
    aRun[nRun].zSql = pBatch->aTxns[i].zSql; aRun[nRun].mode = pBatch->aTxns[i].mode;
    aRun[nRun].now = pBatch->aTxns[i].now;
    aRun[nRun].frag = pBatch->aTxns[i].frag;
    aRun[nRun].group = pBatch->aTxns[i].txn;
    nRun++;
  }
  if (pBatch->contTxn && nCarry == 0 && firstEnd > 0) {
    pCtx->nTxnsDropped++;
    hpplite_error(pCtx, "batch %llu continues a transaction this replica does not hold; "
            "its %d statements here are dropped with it", (unsigned long long)pBatch->height, firstEnd);
  }
  /* Pieces of a statement (#59) become the statement once its last piece is
  ** here; pieces whose statement continues in the next batch stay pieces,
  ** held with their open group */
  {
    int r = 0, k = 0;
    while (r < nRun) {
      int e = r;
      if (!aRun[r].frag) { aRun[k++] = aRun[r++]; continue; }
      while (e < nRun && aRun[e].frag) e++;
      if (e == nRun) { while (r < nRun) aRun[k++] = aRun[r++]; break; }
      {
        sqlite3_str *js = sqlite3_str_new(NULL);
        char *z;
        int m;
        for (m = r; m <= e; m++) sqlite3_str_appendall(js, aRun[m].zSql ? aRun[m].zSql : "");
        z = sqlite3_str_finish(js);
        if (z) {
          char **a2 = sqlite3_realloc64(azJoined, sizeof(char*) * (size_t)(nJoined + 1));
          if (a2) { azJoined = a2; azJoined[nJoined++] = z; } else { sqlite3_free(z); z = NULL; }
        }
        aRun[k] = aRun[e];
        aRun[k].zSql = z;
        aRun[k].frag = 0;
        k++;
        r = e + 1;
      }
    }
    nRun = k;
  }
  /* An open last group is held back for the next batch */
  holdFrom = nRun;
  if (pBatch->openTxn && nRun > 0) {
    if (pBatch->contTxn && nCarry == 0 && firstEnd == pBatch->nTxns) {
      holdFrom = nRun;          /* the headless continuation is all there is: nothing to hold */
    } else {
      holdFrom = nRun - 1;
      while (holdFrom > 0 && aRun[holdFrom - 1].group == aRun[nRun - 1].group) holdFrom--;
    }
  }
  nHold = nRun - holdFrom;
  if (nHold > 0) carryChanged = 1;

  /* The clock: each statement reads the time it read on the writer; one
  ** that did not read it there gets a fixed time (the batch's), so an
  ** unexpected read is at least the same on every replica (#56) */
  for (i = 0; i < holdFrom; i++) {
    if (aRun[i].now && !hpplite_clock_on(rdb)) {
      hpplite_error(pCtx, "batch %llu has statements that read the clock, and the replay connection "
              "is not on the clock VFS; cannot reproduce them", (unsigned long long)pBatch->height);
      for (i = 0; i < nCarry; i++) sqlite3_free(azCarry[i]);
      sqlite3_free(azCarry); sqlite3_free(aCarryMode); sqlite3_free(aCarryNow); sqlite3_free(aCarryFrag);
    for (i = 0; i < nJoined; i++) sqlite3_free(azJoined[i]);
    sqlite3_free(azJoined);
      sqlite3_free(aRun);
      return SQLITE_MISMATCH;
    }
  }
  savedPin = hpplite_clock_pinned();
  savedSeen = hpplite_clock_consulted();

  savedEnabled = pCtx->bEnabled;
  if (rdb == pCtx->db) { pCtx->bEnabled = 0; pCtx->bInternal = 1; }
  int savedMode = connectionMode(rdb), curMode = savedMode;
  char *zTempStash = NULL;
  int tempLooked = 0;                     /* the connection's TEMP triggers set aside for records (#76) */
  for (i = 0; i < holdFrom && rc == SQLITE_OK; i++) {
    const char *zSql = aRun[i].zSql;
    /* A row record (#19) runs under the writer's foreign keys (a cascade it
    ** did is in the records before the change that caused it, so here it
    ** finds nothing left) and leaves recursive_triggers as it is: triggers
    ** are off for it, their effects are records of their own */
    int isRow = (aRun[i].mode & HPPLITE_MODE_ROW) != 0;
    int mode = isRow ? (aRun[i].mode & (HPPLITE_MODE_FOREIGN_KEYS | HPPLITE_MODE_PRAGMAS)) | (curMode & HPPLITE_MODE_RECURSIVE_TRIGGERS)
                     : aRun[i].mode;
    if (!zSql) continue;
    if (useTxn && aRun[i].group != lastTxn) {
      int j, hasRow = 0;
      if (open) rc = sqlite3_exec(rdb, "COMMIT", NULL, NULL, NULL);
      open = 0;
      /* foreign keys for the whole group, before it begins */
      if (rc == SQLITE_OK) rc = replayMode(rdb, mode, &curMode);
      if (rc == SQLITE_OK) rc = sqlite3_exec(rdb, "BEGIN", NULL, NULL, NULL);
      open = (rc == SQLITE_OK);
      lastTxn = aRun[i].group;
      /* records apply a cascade's effects before its cause (the child row
      ** moves to a key its parent has only after the next record): their
      ** foreign keys are checked at the group's end, where the writer's
      ** state held */
      for (j = i; j < holdFrom && aRun[j].group == lastTxn; j++) if (aRun[j].mode & HPPLITE_MODE_ROW) hasRow = 1;
      if (open && hasRow) rc = sqlite3_exec(rdb, "PRAGMA defer_foreign_keys = ON", NULL, NULL, NULL);
    }
    if (rc == SQLITE_OK) rc = replayMode(rdb, mode, &curMode);
    hpplite_clock_pin(aRun[i].now ? aRun[i].now
                                  : (sqlite3_int64)pBatch->timestamp * 1000 + 210866760000000LL);
    if (isRow && rc == SQLITE_OK) {
      char *zErr = NULL;
      int trig = 1;
      if (!tempLooked) { zTempStash = tempTriggersStash(rdb); tempLooked = 1; }
      sqlite3_db_config(rdb, SQLITE_DBCONFIG_ENABLE_TRIGGER, 0, &trig);
      rc = hpplite_rows_apply_entry(rdb, zSql, &zErr);
      sqlite3_db_config(rdb, SQLITE_DBCONFIG_ENABLE_TRIGGER, 1, &trig);
      if (rc != SQLITE_OK) {
        /* a record is what a write that succeeded did: one that cannot be
        ** applied means the replica is not where the writer was */
        hpplite_error(pCtx, "batch %llu row record %d cannot be applied: %s",
                (unsigned long long)pBatch->height, i, zErr ? zErr : sqlite3_errmsg(rdb));
        sqlite3_free(zErr);
        rc = SQLITE_MISMATCH;
        break;
      }
      continue;
    }
    if (zTempStash) { tempTriggersRestore(rdb, &zTempStash); tempLooked = 0; }   /* text fires them */
    if (rc == SQLITE_OK && sqlite3_strnicmp(zSql, "VACUUM", 6) == 0 && zSql[6] == 0) {
      /* not inside a transaction: the group before it ends here */
      if (open) { rc = sqlite3_exec(rdb, "COMMIT", NULL, NULL, NULL); open = 0; lastTxn = -1; }
      if (rc == SQLITE_OK) rc = sqlite3_exec(rdb, "VACUUM", NULL, NULL, NULL);
      continue;
    }
    if (rc == SQLITE_OK) rc = sqlite3_exec(rdb, zSql, NULL, NULL, NULL);
    if (sqlite3_strnicmp(zSql, "CREATE", 6) == 0 && strcasestr(zSql, "TRIGGER")) tempLooked = 0;   /* a new one, maybe TEMP */
    if (rc == SQLITE_CONSTRAINT || rc == SQLITE_ERROR) {
      /* The statement failed here as it did at the sequencer (a failed
      ** statement has no effect, so it is skipped there and skipped here;
      ** capture removes most of them, but not one whose triggers changed
      ** rows before it aborted). A statement-level failure leaves an open
      ** transaction usable; anything that rolled it back is fatal. */
      if (open && sqlite3_get_autocommit(rdb)) break;
      pCtx->nReplaySkipped++;
      hpplite_error(pCtx, "batch %llu statement %d failed on replay (%s); skipped",
              (unsigned long long)pBatch->height, i, sqlite3_errmsg(rdb));
      rc = SQLITE_OK;
    }
  }
  if (open) {
    if (rc == SQLITE_OK) rc = sqlite3_exec(rdb, "COMMIT", NULL, NULL, NULL);
    else sqlite3_exec(rdb, "ROLLBACK", NULL, NULL, NULL);
  }
  tempTriggersRestore(rdb, &zTempStash);  /* after a ROLLBACK they are back already: "exists" is fine */
  hpplite_clock_restore(savedPin, savedSeen);
  /* back to the connection's own state (the application's, on :memory:) */
  if (replayMode(rdb, savedMode, &curMode) != SQLITE_OK) {
    hpplite_error(pCtx, "could not restore the connection's foreign_keys/recursive_triggers after replay");
  }
  if (rdb == pCtx->db) { pCtx->bInternal = 0; pCtx->bEnabled = savedEnabled; }
  pCtx->bRootDirty = 1;
  if (rc != SQLITE_OK) {
    hpplite_error(pCtx, "replay of batch %llu failed at statement %d (rc=%d): %s",
            (unsigned long long)pBatch->height, i, rc, sqlite3_errmsg(rdb));
  } else {
    rc = bkBegin(pCtx, 1);
    if (rc == SQLITE_OK) {
      int saved = bkEnter(pCtx);
      /* the held part moves with the height, in one transaction */
      if (carryChanged) rc = storeCarry(pCtx, aRun + holdFrom, nHold);
      if (rc == SQLITE_OK) rc = advanceMeta(pCtx, pBatch, 0, 0);
      bkLeave(pCtx, saved);
      rc = bkEnd(pCtx, rc);
      if (rc == SQLITE_OK) pCtx->nCarry = nHold;
      else loadMeta(pCtx);              /* memory follows the file (#97) */
    }
  }
  for (i = 0; i < nCarry; i++) sqlite3_free(azCarry[i]);
  sqlite3_free(azCarry); sqlite3_free(aCarryMode); sqlite3_free(aCarryNow); sqlite3_free(aCarryFrag);
    for (i = 0; i < nJoined; i++) sqlite3_free(azJoined[i]);
    sqlite3_free(azJoined);
  sqlite3_free(aRun);
  if (rc != SQLITE_OK) return rc;

  /* The seed of `hpplite verify`: does the content match what was posted?
  ** Only batches that carry a root (checkpoint boundaries) can be checked. */
  if (!pBatch->hasRoot) return SQLITE_OK;
  hpplite_get_state_root(pCtx, root);
  pCtx->nRootsCompared++;
  pCtx->rootCmpHeight = pBatch->height;        /* for the verifier: compared with the on-chain checkpoint (#5) */
  memcpy(pCtx->rootCmp, root, HPPLITE_HASH_SIZE);
  if (memcmp(root, pBatch->postStateRoot, HPPLITE_HASH_SIZE) != 0) {
    pCtx->nRootMismatches++;
    hpplite_error(pCtx, "state root mismatch after replaying batch %llu",
            (unsigned long long)pBatch->height);
  }
  return SQLITE_OK;
}

int hpplite_adopt_batch(HppliteCtx *pCtx, const HppliteBatch *pBatch) {
  sqlite3_stmt *pStmt;
  sqlite3_int64 maxId = 0, lastGroup = 0;
  int rc, i = 0, ok = 1;
  HppliteBatch adopted;

  if (!pCtx || !pBatch) return SQLITE_MISUSE;
  if (pBatch->height != pCtx->blockHeight) return SQLITE_MISMATCH;

  int saved = bkEnter(pCtx);
  /* row records (#19) compare as the entry text the batch was built with */
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, sql, mode, CASE WHEN txn = 0 THEN id ELSE txn END, now, op, tbl, rec FROM _hpplite_pending ORDER BY id LIMIT ?1",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, sql, mode, CASE WHEN txn = 0 THEN id ELSE txn END, now FROM _hpplite_pending ORDER BY id LIMIT ?1",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) { bkLeave(pCtx, saved); return rc; }
  sqlite3_bind_int(pStmt, 1, pBatch->nTxns);
  sqlite3_int64 fragRow = 0;
  size_t fragOff = 0;
  char *zEntry = NULL;
  while (sqlite3_step(pStmt) == SQLITE_ROW && i < pBatch->nTxns) {
    sqlite3_int64 id = sqlite3_column_int64(pStmt, 0);
    const char *zSql = (const char*)sqlite3_column_text(pStmt, 1);
    sqlite3_free(zEntry);
    zEntry = NULL;
    if (sqlite3_column_count(pStmt) > 5 && sqlite3_column_int(pStmt, 5) > 0) {
      zEntry = hpplite_rows_entry(sqlite3_column_int(pStmt, 5), (const char*)sqlite3_column_text(pStmt, 6),
                                  (const unsigned char*)sqlite3_column_blob(pStmt, 7), sqlite3_column_bytes(pStmt, 7));
      zSql = zEntry;
    }
    const char *zRow = zSql;
    const HppliteTxn *t = &pBatch->aTxns[i];
    /* the statement a previous batch posted part of: compare from where it stopped (#59) */
    if (zRow && i == 0 && pCtx->fragId && id == pCtx->fragId && pCtx->fragOff <= strlen(zRow)) zRow += pCtx->fragOff;
    if (!zRow || !t->zSql
        || (t->frag ? (i != pBatch->nTxns - 1 || strncmp(zRow, t->zSql, strlen(t->zSql)) != 0)
                    : strcmp(zRow, t->zSql) != 0)
        || sqlite3_column_int(pStmt, 2) != t->mode
        /* a time in the batch must be the one logged; none means the writer
        ** knew the statement did not read the clock (#56) */
        || (t->now && sqlite3_column_int64(pStmt, 4) != t->now)) {
      ok = 0;
      break;
    }
    lastGroup = sqlite3_column_int64(pStmt, 3);
    if (t->frag) {
      /* a piece: its row stays pending, posted this far */
      fragRow = id;
      fragOff = (size_t)(zRow - zSql) + strlen(t->zSql);
    } else {
      maxId = id;
    }
    i++;
  }
  sqlite3_free(zEntry);
  sqlite3_finalize(pStmt);
  bkLeave(pCtx, saved);
  if (!ok || i != pBatch->nTxns) return SQLITE_MISMATCH;

  /* what it left open is the group of its last statement (#57) */
  adopted = *pBatch;
  adopted.openGroup = pBatch->openTxn ? lastGroup : 0;
  adopted.fragRow = fragRow;
  adopted.fragOff = fragOff;
  return hpplite_finalize_batch(pCtx, &adopted, maxId);
}

int hpplite_pending_count(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int n = 0;
  if (!pCtx) return 0;
  int saved = bkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT count(*) FROM _hpplite_pending",
                         -1, &pStmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) n = sqlite3_column_int(pStmt, 0);
    sqlite3_finalize(pStmt);
  }
  bkLeave(pCtx, saved);
  return n;
}

uint64_t hpplite_get_block_height(HppliteCtx *pCtx) {
  return pCtx ? pCtx->blockHeight : 0;
}

void hpplite_get_prev_block_hash(HppliteCtx *pCtx, unsigned char *pOut) {
  if (pCtx && pOut) {
    memcpy(pOut, pCtx->prevBlockHash, HPPLITE_HASH_SIZE);
  }
}

int hpplite_skip_height(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRawHash) {
  sqlite3_stmt *pStmt;
  int rc;
  if (!pCtx || !pRawHash) return SQLITE_MISUSE;
  if (height != pCtx->blockHeight) return SQLITE_MISMATCH;
  rc = bkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  {
    int saved = bkEnter(pCtx);
    rc = sqlite3_prepare_v2(pCtx->bkDb,
      "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2 WHERE id=1", -1, &pStmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)(height + 1));
      sqlite3_bind_blob(pStmt, 2, pRawHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
      rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
      sqlite3_finalize(pStmt);
    }
    bkLeave(pCtx, saved);
  }
  rc = bkEnd(pCtx, rc);
  if (rc == SQLITE_OK) {
    pCtx->blockHeight = height + 1;
    memcpy(pCtx->prevBlockHash, pRawHash, HPPLITE_HASH_SIZE);
    pCtx->nForeignBatches++;
  }
  return rc;
}

int hpplite_set_checkpoint(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRoot) {
  sqlite3_stmt *pStmt;
  int rc;
  if (!pCtx || !pRoot) return SQLITE_MISUSE;
  int saved = bkEnter(pCtx);
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "UPDATE _hpplite_meta SET last_checkpoint_height=?1, last_checkpoint_root=?2 WHERE id=1",
    -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height);
    sqlite3_bind_blob(pStmt, 2, pRoot, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_finalize(pStmt);
  }
  bkLeave(pCtx, saved);
  if (rc == SQLITE_OK) {
    pCtx->lastCheckpointHeight = height;
    memcpy(pCtx->lastCheckpointRoot, pRoot, HPPLITE_HASH_SIZE);
  }
  return rc;
}

/*
** ============================================================================
** Simple Open API (M3)
** ============================================================================
*/

#include "config.h"
#include "crypto.h"
#include "batch.h"
#include "l1_interface.h"
#include "node.h"

/*
** Global registry mapping sqlite3* -> HppliteNode
** (Simple linked list for now, could use hash table for performance)
*/
typedef struct NodeEntry NodeEntry;
struct NodeEntry {
    sqlite3 *db;
    HppliteNode *node;
    HppliteConfig *config;  /* Stored for hpplite_get_config() */
    NodeEntry *next;
};

static NodeEntry *g_nodeRegistry = NULL;

/* Connections open and close on any thread; the list is read from SQL
** functions and the status table. One static SQLite mutex guards it. */
static sqlite3_mutex *regMutex(void) {
    return sqlite3_mutex_alloc(SQLITE_MUTEX_STATIC_APP1);
}

static NodeEntry *findEntryLocked(sqlite3 *db) {
    NodeEntry *e = g_nodeRegistry;
    while (e) {
        if (e->db == db) return e;
        e = e->next;
    }
    return NULL;
}

static NodeEntry *findEntry(sqlite3 *db) {
    sqlite3_mutex *m = regMutex();
    NodeEntry *e;
    sqlite3_mutex_enter(m);
    e = findEntryLocked(db);
    sqlite3_mutex_leave(m);
    return e;
}

static HppliteNode *findNode(sqlite3 *db) {
    NodeEntry *e = findEntry(db);
    return e ? e->node : NULL;
}

static void removeNode(sqlite3 *db) {
    sqlite3_mutex *m = regMutex();
    NodeEntry **pp, *e = NULL;
    sqlite3_mutex_enter(m);
    pp = &g_nodeRegistry;
    while (*pp) {
        if ((*pp)->db == db) {
            e = *pp;
            *pp = e->next;
            break;
        }
        pp = &(*pp)->next;
    }
    sqlite3_mutex_leave(m);
    /* Note: node is destroyed separately via hpplite_node_destroy() */
    if (e) {
        if (e->config) hpplite_config_free(e->config);
        free(e);
    }
}

/* Forward declarations for SQL functions */
static void hpplite_sync_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_flush_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);

static void hpplite_on_close(sqlite3 *db);

/*
** ============================================================================
** hpplite_status: eponymous virtual table
** ============================================================================
**
**   SELECT * FROM hpplite_status;
**
** One row: height, pending, state_root, prev_block_hash,
** last_checkpoint_height, lease, log_errors, replay_skipped, encryption,
** keyed, nondeterministic, contract, da_contract, last_batch_tx, foreign_batches,
** hooks_replaced, roots_compared, root_mismatches, txns_dropped, last_refusal,
** sequencer (1 if this connection posts the file's batches; 0 if another
** connection, in this process or another, does and this one captures only),
** capture ('rows' or 'text': what this connection logs).
**
** The table is referenced once when the node is registered so that an
** instance exists for the life of the connection; its xDisconnect then fires
** inside sqlite3_close() and performs the HPPLite shutdown (flush, release
** lease, free). No change to SQLite core is needed for this.
*/
typedef struct StatusVtab {
    sqlite3_vtab base;
    sqlite3 *db;
} StatusVtab;

typedef struct StatusCursor {
    sqlite3_vtab_cursor base;
    int eof;
} StatusCursor;

#define STATUS_COL_HEIGHT 0
#define STATUS_COL_PENDING 1
#define STATUS_COL_STATE_ROOT 2
#define STATUS_COL_PREV_BLOCK_HASH 3
#define STATUS_COL_LAST_CHECKPOINT 4
#define STATUS_COL_LEASE 5
#define STATUS_COL_LOG_ERRORS 6
#define STATUS_COL_REPLAY_SKIPPED 7
#define STATUS_COL_ENCRYPTION 8
#define STATUS_COL_KEYED 9
#define STATUS_COL_NONDETERMINISTIC 10
#define STATUS_COL_CONTRACT         11
#define STATUS_COL_DA_CONTRACT      12
#define STATUS_COL_LAST_BATCH_TX    13
#define STATUS_COL_FOREIGN_BATCHES  14
#define STATUS_COL_HOOKS_REPLACED   15
#define STATUS_COL_ROOTS_COMPARED   16
#define STATUS_COL_ROOT_MISMATCHES  17
#define STATUS_COL_TXNS_DROPPED     18
#define STATUS_COL_LAST_REFUSAL     19
#define STATUS_COL_SEQUENCER        20
#define STATUS_COL_CAPTURE          21
#define STATUS_COL_FOREIGN_WRITES   22
#define STATUS_COL_STATE            23
#define STATUS_COL_LAST_ERROR       24

static int statusConnect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
                         sqlite3_vtab **ppVtab, char **pzErr) {
    StatusVtab *p;
    int rc;
    (void)pAux; (void)argc; (void)argv; (void)pzErr;
    rc = sqlite3_declare_vtab(db,
        "CREATE TABLE x(height INTEGER, pending INTEGER, state_root TEXT,"
        " prev_block_hash TEXT, last_checkpoint_height INTEGER, lease INTEGER,"
        " log_errors INTEGER, replay_skipped INTEGER, encryption TEXT, keyed INTEGER,"
        " nondeterministic INTEGER, contract TEXT, da_contract TEXT, last_batch_tx TEXT,"
        " foreign_batches INTEGER, hooks_replaced INTEGER, roots_compared INTEGER, root_mismatches INTEGER,"
        " txns_dropped INTEGER, last_refusal TEXT, sequencer INTEGER, capture TEXT, foreign_writes INTEGER,"
        " state TEXT, last_error TEXT)");
    if (rc != SQLITE_OK) return rc;
    p = sqlite3_malloc(sizeof(StatusVtab));
    if (!p) return SQLITE_NOMEM;
    memset(p, 0, sizeof(StatusVtab));
    p->db = db;
    *ppVtab = &p->base;
    return SQLITE_OK;
}

static int statusDisconnect(sqlite3_vtab *pVtab) {
    StatusVtab *p = (StatusVtab *)pVtab;
    hpplite_on_close(p->db);
    sqlite3_free(p);
    return SQLITE_OK;
}

/*
** Destructor of the sentinel SQL function: SQLite calls it only when the
** connection really closes (after the unfinalized-statement check that
** xDisconnect runs before). A node still registered here belongs to a
** connection whose earlier close attempt failed with SQLITE_BUSY; tear it
** down now, without touching the dead connection.
*/
static void closeSentinelDestroy(void *pArg) {
    sqlite3 *db = (sqlite3 *)pArg;
    HppliteNode *node = findNode(db);
    if (!node) return;
    removeNode(db);
    node->ownsDb = 0;
    if (node->ctx) node->ctx->bDbClosed = 1;
    hpplite_node_destroy(node);
}
static void closeSentinelFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc; (void)argv;
    sqlite3_result_null(ctx);
}

static int statusBestIndex(sqlite3_vtab *pVtab, sqlite3_index_info *pInfo) {
    (void)pVtab;
    pInfo->estimatedCost = 1;
    pInfo->estimatedRows = 1;
    return SQLITE_OK;
}

static int statusOpen(sqlite3_vtab *pVtab, sqlite3_vtab_cursor **ppCur) {
    StatusCursor *c = sqlite3_malloc(sizeof(StatusCursor));
    (void)pVtab;
    if (!c) return SQLITE_NOMEM;
    memset(c, 0, sizeof(StatusCursor));
    *ppCur = &c->base;
    return SQLITE_OK;
}

static int statusClose(sqlite3_vtab_cursor *pCur) {
    sqlite3_free(pCur);
    return SQLITE_OK;
}

static int statusFilter(sqlite3_vtab_cursor *pCur, int idxNum, const char *idxStr,
                        int argc, sqlite3_value **argv) {
    {   /* application thread: a cheap moment to check our hooks are still ours */
        HppliteNode *n = findNode(((StatusVtab *)pCur->pVtab)->db);
        if (n && n->ctx) hpplite_rearm_hooks(n->ctx);
        if (n && n->ctx && n->follower) hpplite_refresh_meta(n->ctx);   /* the sequencer moved the file (#100) */
    }
    (void)idxNum; (void)idxStr; (void)argc; (void)argv;
    ((StatusCursor *)pCur)->eof = 0;
    return SQLITE_OK;
}

static int statusNext(sqlite3_vtab_cursor *pCur) {
    ((StatusCursor *)pCur)->eof = 1;
    return SQLITE_OK;
}

static int statusEof(sqlite3_vtab_cursor *pCur) {
    return ((StatusCursor *)pCur)->eof;
}

static void statusResultHex(sqlite3_context *ctx, const unsigned char *p) {
    char hex[HPPLITE_HASH_SIZE * 2 + 3];
    hex[0] = '0'; hex[1] = 'x';
    hpplite_to_hex(p, HPPLITE_HASH_SIZE, hex + 2);
    sqlite3_result_text(ctx, hex, -1, SQLITE_TRANSIENT);
}

static int statusColumn(sqlite3_vtab_cursor *pCur, sqlite3_context *ctx, int col) {
    StatusVtab *p = (StatusVtab *)pCur->pVtab;
    HppliteNode *node = findNode(p->db);
    unsigned char root[HPPLITE_HASH_SIZE];
    if (!node || !node->ctx) {
        sqlite3_result_null(ctx);
        return SQLITE_OK;
    }
    switch (col) {
        case STATUS_COL_HEIGHT:
            sqlite3_result_int64(ctx, (sqlite3_int64)hpplite_get_block_height(node->ctx));
            break;
        case STATUS_COL_PENDING:
            sqlite3_result_int(ctx, hpplite_pending_count(node->ctx));
            break;
        case STATUS_COL_STATE_ROOT:
            hpplite_get_state_root(node->ctx, root);
            statusResultHex(ctx, root);
            break;
        case STATUS_COL_PREV_BLOCK_HASH:
            statusResultHex(ctx, node->ctx->prevBlockHash);
            break;
        case STATUS_COL_LAST_CHECKPOINT:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->lastCheckpointHeight);
            break;
        case STATUS_COL_LEASE:
            sqlite3_result_int(ctx, node->hasLease);
            break;
        case STATUS_COL_LOG_ERRORS:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nLogErrors);
            break;
        case STATUS_COL_REPLAY_SKIPPED:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nReplaySkipped);
            break;
        case STATUS_COL_ENCRYPTION:
            sqlite3_result_text(ctx, node->cipher ? node->cipher->name : "none", -1, SQLITE_STATIC);
            break;
        case STATUS_COL_KEYED:
            sqlite3_result_int(ctx, node->config.hasPrivkey ? 1 : 0);
            break;
        case STATUS_COL_NONDETERMINISTIC:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nNondeterministic);
            break;
        case STATUS_COL_CONTRACT:
        case STATUS_COL_DA_CONTRACT: {
            char c[43], d[43];
            if (node->l1 && hpplite_l1_get_addresses(node->l1, c, d) == 0) {
                const char *z = (col == STATUS_COL_CONTRACT) ? c : d;
                if (z[0]) sqlite3_result_text(ctx, z, -1, SQLITE_TRANSIENT); else sqlite3_result_null(ctx);
            } else {
                sqlite3_result_null(ctx);
            }
            break;
        }
        case STATUS_COL_FOREIGN_BATCHES:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nForeignBatches);
            break;
        case STATUS_COL_HOOKS_REPLACED:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nHooksReplaced);
            break;
        case STATUS_COL_ROOTS_COMPARED:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nRootsCompared);
            break;
        case STATUS_COL_ROOT_MISMATCHES:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nRootMismatches);
            break;
        case STATUS_COL_TXNS_DROPPED:
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nTxnsDropped);
            break;
        case STATUS_COL_LAST_REFUSAL:
            if (node->ctx->zLastRefusal[0]) sqlite3_result_text(ctx, node->ctx->zLastRefusal, -1, SQLITE_TRANSIENT);
            else sqlite3_result_null(ctx);
            break;
        case STATUS_COL_SEQUENCER:
            sqlite3_result_int(ctx, node->follower ? 0 : 1);
            break;
        case STATUS_COL_CAPTURE:              /* what this connection logs: records or statements (#83) */
            sqlite3_result_text(ctx, node->ctx->bRows ? "rows" : "text", -1, SQLITE_STATIC);
            break;
        case STATUS_COL_FOREIGN_WRITES:       /* the file changed with no HPPLite write to show for it (#37) */
            sqlite3_result_int64(ctx, (sqlite3_int64)node->ctx->nForeignWrites);
            break;
        case STATUS_COL_STATE: {              /* what this node is doing about posting (#32) */
            const char *z;
            if (node->state == HPPLITE_STATE_ERROR) z = "error";
            else if (node->config.verifyOnly) z = "verify";
            else if (node->state == HPPLITE_STATE_STOPPED) z = "stopped";
            else if (node->state == HPPLITE_STATE_INIT) z = "init";
            else if (!node->l1) z = node->wantsL1 ? "disconnected" : node->config.localOnly ? "local" : "unposted";
            else if (node->follower) z = "follower";
            else if (node->leaseHeldElsewhere) z = "waiting";
            else if (node->retryAt) z = "retrying";
            else z = "sequencing";
            sqlite3_result_text(ctx, z, -1, SQLITE_STATIC);
            break;
        }
        case STATUS_COL_LAST_ERROR:           /* the last failure, until the next success (#32) */
            if (node->ctx->zLastError[0]) sqlite3_result_text(ctx, node->ctx->zLastError, -1, SQLITE_TRANSIENT);
            break;
        case STATUS_COL_LAST_BATCH_TX:
            if (node->lastBatchTx[0]) sqlite3_result_text(ctx, node->lastBatchTx, -1, SQLITE_TRANSIENT);
            else sqlite3_result_null(ctx);
            break;
        default:
            sqlite3_result_null(ctx);
    }
    return SQLITE_OK;
}

static int statusRowid(sqlite3_vtab_cursor *pCur, sqlite3_int64 *pRowid) {
    (void)pCur;
    *pRowid = 1;
    return SQLITE_OK;
}

static sqlite3_module hpplite_status_module = {
    0,                  /* iVersion */
    0,                  /* xCreate: eponymous only */
    statusConnect,
    statusBestIndex,
    statusDisconnect,
    0,                  /* xDestroy */
    statusOpen,
    statusClose,
    statusFilter,
    statusNext,
    statusEof,
    statusColumn,
    statusRowid,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

/*
** Register a node with the global registry and set up the SQL surface:
** hpplite_sync(), hpplite_flush() and the hpplite_status table.
** Called by hpplite_node_create() and hpplite_node_create_with_db().
*/
HppliteNode *hpplite_find_sequencer(const char *zCanonicalPath) {
    sqlite3_mutex *m = regMutex();
    NodeEntry *e;
    HppliteNode *found = NULL;
    if (!zCanonicalPath) return NULL;
    sqlite3_mutex_enter(m);
    for (e = g_nodeRegistry; e; e = e->next) {
        if (e->node && !e->node->follower && e->node->canonicalPath
            && strcmp(e->node->canonicalPath, zCanonicalPath) == 0) {
            found = e->node;
            break;
        }
    }
    sqlite3_mutex_leave(m);
    return found;
}

void hpplite_register_node(sqlite3 *db, HppliteNode *node) {
    if (!db || !node) return;

    /* Add to registry (once per connection) */
    NodeEntry *entry = calloc(1, sizeof(NodeEntry));
    if (!entry) return;
    entry->db = db;
    entry->node = node;
    entry->config = NULL;  /* Filled in by hpplite_open() / auto-init */
    {
        sqlite3_mutex *m = regMutex();
        sqlite3_mutex_enter(m);
        if (findEntryLocked(db)) {
            sqlite3_mutex_leave(m);
            free(entry);
            return;
        }
        entry->next = g_nodeRegistry;
        g_nodeRegistry = entry;
        sqlite3_mutex_leave(m);
    }

    /* Register SQL functions */
    sqlite3_create_function(db, "hpplite_sync", 0, SQLITE_UTF8, NULL,
                            hpplite_sync_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_flush", 0, SQLITE_UTF8, NULL,
                            hpplite_flush_func, NULL, NULL);
    sqlite3_create_function_v2(db, "_hpplite_close_sentinel", 0, SQLITE_UTF8, db,
                               closeSentinelFunc, NULL, NULL, closeSentinelDestroy);

    /* Status table; touch it once so its instance lives until close */
    if (sqlite3_create_module(db, "hpplite_status", &hpplite_status_module, NULL) == SQLITE_OK) {
        sqlite3_exec(db, "SELECT * FROM hpplite_status LIMIT 0", NULL, NULL, NULL);
    }
}

/*
** Connection close. Runs from the hpplite_status virtual table's xDisconnect,
** which SQLite invokes at the start of sqlite3_close() / sqlite3_close_v2()
** while the connection is still fully usable - the same point a close hook
** in core would give us, without patching core.
*/
static void hpplite_on_close(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    if (!node) return;

    /* Our own prepared statements would make the close fail; the trace
    ** callback re-prepares them if the connection lives on */
    if (node->ctx) { rearmHooks(node->ctx, 1); finalizeOwnStatements(node->ctx); }

    /* SQLite disconnects virtual tables before it checks for unfinalized
    ** statements. If the application still has one, this close is about to
    ** fail with SQLITE_BUSY and the connection stays in use: flush what is
    ** pending but keep capturing. The sentinel destructor finishes the
    ** teardown when the connection really closes (issue #31). */
    if (node->ctx && applicationStatementsOpen(node->ctx)) {
        hpplite_node_flush_batch(node);
        node->ctx->bStatusGone = 1;
        return;
    }

    /* Unregister first: the teardown below closes connections and must not
    ** find this node again. */
    removeNode(db);

    /*
    ** IMPORTANT: Set ownsDb to 0 because sqlite3_close() is already closing
    ** the database. We don't want hpplite_node_destroy() to try to close it
    ** again, which would cause a double-free/double-close bug.
    */
    node->ownsDb = 0;

    /* Destroy the node (stops timer, flushes pending, frees resources) */
    hpplite_node_destroy(node);
}

void hpplite_unregister_node(sqlite3 *db) {
    removeNode(db);
}

/*
** Helper: convert HppliteConfig to HppliteNodeConfig
*/
static void config_to_node_config(const HppliteConfig *cfg, HppliteNodeConfig *nc) {
    memset(nc, 0, sizeof(HppliteNodeConfig));
    nc->dataDir = cfg->dataDir ? strdup(cfg->dataDir) : NULL;
    nc->dbPath = cfg->dbPath ? strdup(cfg->dbPath) : NULL;
    nc->rpcUrl = cfg->rpcUrl ? strdup(cfg->rpcUrl) : NULL;
    memcpy(nc->privkey, cfg->privkey, 32);
    nc->hasPrivkey = cfg->hasPrivkey;
    memcpy(nc->factory, cfg->factory, 20);
    nc->hasFactory = cfg->hasFactory;
    memcpy(nc->contract, cfg->contract, 20);
    nc->hasContract = cfg->hasContract;
    nc->chainId = cfg->chainId;
    nc->batchIntervalMs = cfg->batchIntervalMs;
    nc->noEncrypt = !cfg->encrypt;
    nc->determinismWarn = cfg->determinismWarn;
    nc->captureText = cfg->captureText;
    nc->captureSource = cfg->captureSource;
    nc->localOnly = cfg->localOnly;
    nc->verifyOnly = cfg->role == HPPLITE_CFG_ROLE_VERIFY;
    nc->dbName = cfg->dbName;
    nc->maxTxBytes = cfg->maxTxBytes;
    nc->maxTxGas = cfg->maxTxGas;
    nc->checkpointInterval = cfg->checkpointInterval;

    /* Role removed - always sequencer in simplified model */
}

/*
** Helper: free node config strings
*/
static void free_node_config(HppliteNodeConfig *nc) {
    free(nc->dataDir);
    free(nc->dbPath);
    free(nc->rpcUrl);
    free(nc->nodeId);
}

sqlite3 *hpplite_open(const char *uri) {
    if (!uri) return NULL;

    /* Load configuration */
    HppliteConfig *config = hpplite_config_load(uri);
    if (!config) return NULL;
    if (hpplite_config_error()) {
        fprintf(stderr, "hpplite: cannot open %s: %s\n", uri, hpplite_config_error());
        hpplite_config_free(config);
        return NULL;
    }

    /* Check if hpplite is enabled */
    if (!config->dbPath) {
        hpplite_config_free(config);
        return NULL;
    }

    /* Convert to node config */
    HppliteNodeConfig nodeConfig;
    config_to_node_config(config, &nodeConfig);

    /* Create node (opens db internally) */
    HppliteNode *node = hpplite_node_create(&nodeConfig);
    free_node_config(&nodeConfig);

    if (!node) {
        hpplite_config_free(config);
        return NULL;
    }

    sqlite3 *db = node->db;

    /* Node creation registered the connection; keep the config with it */
    NodeEntry *entry = findEntry(db);
    if (!entry) {
        hpplite_config_free(config);
        hpplite_node_destroy(node);
        return NULL;
    }
    entry->config = config;  /* Store config for hpplite_get_config() */

    /* Start auto-flush timer thread */
    hpplite_node_start_timer(node);

    /* Start node (claim lease if L1 connected) */
    hpplite_node_start(node);

    return db;
}

int hpplite_close(sqlite3 *db) {
    /*
    ** sqlite3_close() disconnects the hpplite_status table, whose
    ** xDisconnect performs the HPPLite shutdown. Kept for API compatibility.
    */
    return sqlite3_close(db);
}

HppliteCtx *hpplite_context(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    return node ? node->ctx : NULL;
}

struct HppliteConfig *hpplite_get_config(sqlite3 *db) {
    NodeEntry *e = findEntry(db);
    return e ? e->config : NULL;
}

HppliteL1 *hpplite_get_l1(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    return node ? node->l1 : NULL;
}

HppliteNode *hpplite_get_node(sqlite3 *db) {
    return findNode(db);
}

uint64_t hpplite_flush(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    if (!node) return 0;
    /* Use the node's flush (sequencer path) */
    return hpplite_node_flush_batch(node);
}

/*
** Check if auto-flush is needed and perform it if so.
** Returns batch height if flushed, 0 otherwise.
** Note: With timer thread, this is less necessary but kept for compatibility.
*/
uint64_t hpplite_check_flush(sqlite3 *db) {
    HppliteNode *node = findNode(db);
    if (!node) return 0;
    /* The timer thread handles auto-flush; this is a manual trigger */
    return hpplite_node_flush_batch(node);
}

void hpplite_state_root(sqlite3 *db, unsigned char *out) {
    HppliteNode *node = findNode(db);
    if (node && node->ctx) {
        hpplite_get_state_root(node->ctx, out);
    } else {
        memset(out, 0, HPPLITE_HASH_SIZE);
    }
}

/*
** SQL function: hpplite_sync()
** Triggers sync from L1 for observer/replica nodes.
*/
static void hpplite_sync_func(
    sqlite3_context *ctx,
    int argc,
    sqlite3_value **argv
) {
    (void)argc;
    (void)argv;

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = findNode(db);

    if (!node || !node->l1) {
        sqlite3_result_int(ctx, 0);
        return;
    }

    /* Running inside a statement on this connection: replay without
    ** wrapping each batch transaction in BEGIN/COMMIT. */
    if (node->ctx) { hpplite_rearm_hooks(node->ctx); node->ctx->bAppFlushing = 1; }
    int synced = hpplite_node_sync_from_l1(node, 0);
    if (node->ctx) node->ctx->bAppFlushing = 0;
    sqlite3_result_int(ctx, synced < 0 ? 0 : synced);
}

/*
** SQL function: hpplite_flush()
** Triggers a batch flush for sequencer nodes.
*/
static void hpplite_flush_func(
    sqlite3_context *ctx,
    int argc,
    sqlite3_value **argv
) {
    (void)argc;
    (void)argv;

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = findNode(db);

    if (!node) {
        sqlite3_result_int64(ctx, 0);
        return;
    }

    if (node->ctx) { hpplite_rearm_hooks(node->ctx); node->ctx->bAppFlushing = 1; }
    uint64_t height = hpplite_node_flush_batch(node);
    if (node->ctx) node->ctx->bAppFlushing = 0;
    sqlite3_result_int64(ctx, (sqlite3_int64)height);
}

/*
** Auto-extension callback - called automatically on every sqlite3_open()
** Checks URI parameters and initializes HPPLite if enabled.
** Uses HppliteNode for unified implementation.
*/
/* HPPLITE_FILES: ':'-separated glob patterns, matched against the full
** path and the file name (#55) */
static int envNamesFile(const char *zPath) {
    const char *zList = getenv("HPPLITE_FILES");
    const char *zBase = strrchr(zPath, '/');
    char pat[1024];
    zBase = zBase ? zBase + 1 : zPath;
    while (zList && *zList) {
        const char *e = strchr(zList, ':');
        size_t n = e ? (size_t)(e - zList) : strlen(zList);
        if (n > 0 && n < sizeof(pat)) {
            memcpy(pat, zList, n);
            pat[n] = 0;
            if (fnmatch(pat, zPath, 0) == 0 || fnmatch(pat, zBase, 0) == 0) return 1;
            if (!strpbrk(pat, "*?[") && strchr(pat, '/')) {
                /* a plain path: compare where it points (SQLite reports the
                ** resolved path: /tmp is /private/tmp on macOS); the file
                ** may not exist yet, its directory does */
                char dir[1024], res[PATH_MAX];
                const char *slash = strrchr(pat, '/');
                size_t dn = (size_t)(slash - pat);
                if (dn == 0) { dir[0] = '/'; dir[1] = 0; } else { memcpy(dir, pat, dn); dir[dn] = 0; }
                if (realpath(dir, res)) {
                    char full[PATH_MAX + 256];
                    snprintf(full, sizeof(full), "%s%s%s", res, strcmp(res, "/") ? "/" : "", slash + 1);
                    if (strcmp(full, zPath) == 0) return 1;
                }
            }
        }
        zList = e ? e + 1 : NULL;
    }
    return 0;
}

static int hpplite_auto_init(
    sqlite3 *db,
    char **pzErrMsg,
    const sqlite3_api_routines *pApi
) {
    (void)pzErrMsg;
    (void)pApi;

    /* Get the URI used to open this database */
    const char *filename = sqlite3_db_filename(db, "main");
    if (!filename) return SQLITE_OK;
    if (hpplite_internal_open_active()) return SQLITE_OK;   /* our own bookkeeping connection */
    /* A read-only connection (mode=ro, SQLITE_OPEN_READONLY, a file without
    ** write permission) writes nothing to capture: it opens as it would
    ** without HPPLite - a reader beside the application must not fail */
    if (sqlite3_db_readonly(db, "main") == 1) return SQLITE_OK;

    /* Enabled by the URI (hpplite=on), or - for drivers that cannot open
    ** URIs - by HPPLITE_FILES naming this file (#55); a URI that says
    ** hpplite= itself decides */
    const char *hppliteParam = sqlite3_uri_parameter(filename, "hpplite");
    HppliteConfig *config = NULL;
    if (hppliteParam) {
        if (strcmp(hppliteParam, "on") != 0) return SQLITE_OK;
        config = hpplite_config_create();
        if (!config) return SQLITE_OK;
        hpplite_config_load_env(config);                    /* env first */
        {
            /* then HPPLITE_PARAMS, what a URI would carry, for a URI open
            ** too (#86): one setting for every file, the URI's own on top */
            const char *zParams = getenv("HPPLITE_PARAMS");
            if (zParams && zParams[0]) {
                char *zUri = sqlite3_mprintf("file:%s?hpplite=on&%s", filename, zParams);
                if (zUri) { hpplite_config_load_uri(config, zUri); sqlite3_free(zUri); }
            }
        }
        hpplite_config_load_sqlite_uri(config, filename);   /* the URI overrides it */
    } else if (filename[0] && envNamesFile(filename)) {
        const char *zParams = getenv("HPPLITE_PARAMS");
        char *zUri = sqlite3_mprintf("file:%s?hpplite=on%s%s", filename,
                                     zParams && zParams[0] ? "&" : "", zParams ? zParams : "");
        if (!zUri) return SQLITE_OK;
        config = hpplite_config_load(zUri);                 /* env, then HPPLITE_PARAMS */
        sqlite3_free(zUri);
        if (!config) return SQLITE_OK;
    } else {
        return SQLITE_OK;                                   /* not an HPPLite database */
    }
    if (hpplite_config_error()) {
        /* a setting that is not one of its values: not a default in its place (#80) */
        if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("hpplite: cannot start on %s: %s", filename, hpplite_config_error());
        hpplite_config_free(config);
        return SQLITE_ERROR;
    }

    /* Convert to node config */
    HppliteNodeConfig nodeConfig;
    config_to_node_config(config, &nodeConfig);

    /* Create node using existing db connection */
    HppliteNode *node = hpplite_node_create_with_db(db, &nodeConfig);
    free_node_config(&nodeConfig);

    if (!node) {
        /* hpplite=on was asked for and cannot be honoured: fail the open
        ** rather than hand back a connection that silently captures nothing */
        hpplite_config_free(config);
        if (pzErrMsg && hpplite_init_error()) *pzErrMsg = sqlite3_mprintf("hpplite: cannot start on %s: %s", filename, hpplite_init_error());
        else if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("hpplite: cannot start on %s (bad key, unreadable key file, "
                                                  "or bookkeeping tables could not be created)", filename);
        return SQLITE_ERROR;
    }

    /* Node creation registered the connection; keep the config with it */
    NodeEntry *entry = findEntry(db);
    if (!entry) {
        hpplite_config_free(config);
        hpplite_node_destroy(node);
        return SQLITE_OK;
    }
    entry->config = config;  /* Store config for hpplite_get_config() */

    /* Start auto-flush timer thread */
    hpplite_node_start_timer(node);

    return SQLITE_OK;
}

#ifndef HPPLITE_EXTENSION
/*
** Auto-register HPPLite when the library is linked in.
** This uses a constructor attribute so registration happens automatically
** before main() is called.
*/
__attribute__((constructor))
static void hpplite_auto_register(void) {
    hpplite_clock_register();        /* before any connection is opened (#56) */
    sqlite3_auto_extension((void(*)(void))hpplite_auto_init);
}
#endif

/*
** Register HPPLite as an auto-extension.
**
** DEPRECATED: No longer needed. HPPLite auto-registers when the library
** is loaded via constructor. This function is retained for backwards
** compatibility but does nothing.
*/
void hpplite_register(void) {
    /* No-op: auto-registered via constructor */
}

/*
** Unregister HPPLite auto-extension.
**
** DEPRECATED: Retained for backwards compatibility but does nothing.
*/
void hpplite_unregister(void) {
    /* No-op */
}

/*
** Loadable extension entry point:
**
**   sqlite> .load ./hpplite
**   sqlite> .open 'file:app.db?hpplite=on&l1=hpp-sepolia&privkey=0x...'
**
** or sqlite3_load_extension() from any program linked against stock SQLite.
** Activates HPPLite on the loading connection if its URI asks for it, and
** registers the auto-extension so later connections in the process get the
** same treatment.
*/
#ifdef HPPLITE_EXTENSION
SQLITE_EXTENSION_INIT1

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/*
** SQLite unloads a loadable extension when the connection that loaded it is
** closed. HPPLite registers an auto-extension and runs timer threads, so its
** code must stay mapped for the life of the process: take an extra,
** never-released reference to our own image.
*/
static void hpplite_pin_in_memory(void) {
#ifdef _WIN32
  HMODULE h;
  GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                     (LPCSTR)hpplite_pin_in_memory, &h);
#else
  Dl_info info;
  if (dladdr((void*)hpplite_pin_in_memory, &info) && info.dli_fname) {
    (void)dlopen(info.dli_fname, RTLD_NOW | RTLD_NODELETE);
  }
#endif
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_hpplite_init(
  sqlite3 *db,
  char **pzErrMsg,
  const sqlite3_api_routines *pApi
) {
  /* One process, two SQLite libraries (better-sqlite3's bundled copy and
  ** Node's own, say), both loading this module: there is one module, so one
  ** API table and one clock VFS - the second library's connections would
  ** run on the first one's functions. Refuse the second library. */
  if (sqlite3_api && sqlite3_api != pApi) {
    if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("hpplite is already loaded into another SQLite library in this "
                                              "process; one process can use HPPLite with one SQLite library");
    return SQLITE_ERROR;
  }
  SQLITE_EXTENSION_INIT2(pApi);
  /* A second copy of HPPLite (the static library plus this module, or the
  ** module loaded twice) would replace the first one's hpplite_status
  ** module and tear its hooks down: nothing would be captured. */
  {
    sqlite3_stmt *pStmt = NULL;
    int dup = 0;
    if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_module_list WHERE name='hpplite_status'",
                           -1, &pStmt, NULL) == SQLITE_OK) {
      dup = (sqlite3_step(pStmt) == SQLITE_ROW);
      sqlite3_finalize(pStmt);
    }
    /* ...but this copy already on this connection (its auto-extension ran at
    ** open; Rails' `extensions:`, and drivers like it, load the module on
    ** every connection) is a load with nothing left to do */
    if (dup && findNode(db)) return SQLITE_OK;
    if (dup) {
      if (pzErrMsg) *pzErrMsg = sqlite3_mprintf("hpplite is already loaded in this connection");
      return SQLITE_ERROR;
    }
  }
  hpplite_pin_in_memory();
  /* connections opened from here on take their time from the clock VFS
  ** (#56); the module stays loaded, so the VFS outlives every connection */
  hpplite_clock_register();
  sqlite3_auto_extension((void(*)(void))hpplite_auto_init);
  return hpplite_auto_init(db, pzErrMsg, pApi);
}
#endif
