/*
** HPPLite - Verifiable SQLite
**
** Lifecycle of a tracked connection (init, shutdown, the _hpplite_meta
** mirror), the connection registry and the open/close API, the extension
** entry point. The capture engine's other files are listed in hpplite_int.h;
** hpplite.h has the persistence model.
*/

#include "hpplite_int.h"
#include "walread.h"
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
#include <fcntl.h>
#include <sys/file.h>
#include "state_tree.h"

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
  "  rollup TEXT NOT NULL DEFAULT '',"      /* "<chain id>:<contract>" its history is on */
  "  chain_hash BLOB"                       /* the rollup's rolling hash over every batch so far (#33); NULL/zero: not known */
  ");"
  "INSERT OR IGNORE INTO _hpplite_meta(id, next_height, prev_block_hash, state_root)"
  "  VALUES(1, 1, zeroblob(32), zeroblob(32));"
  /* the bytes of the writer's recent batches, to post again if a chain
  ** reorganisation drops them (#49) */
  "CREATE TABLE IF NOT EXISTS _hpplite_posted("
  "  height INTEGER PRIMARY KEY,"
  "  data BLOB NOT NULL"
  ");"
  /* batches a store holds that the chain has not anchored yet (#164): the
  ** next anchor names them, in a manifest, under one commitment */
  "CREATE TABLE IF NOT EXISTS _hpplite_unanchored("
  "  height INTEGER PRIMARY KEY,"
  "  hash BLOB NOT NULL,"          /* keccak256 of the posted bytes */
  "  ref TEXT NOT NULL,"           /* the store's reference */
  "  receipt BLOB,"                /* the store's compact receipt */
  "  chain_before BLOB NOT NULL,"  /* the chain hash before this batch: the manifest starts from the first row's */
  "  chain_after BLOB NOT NULL,"   /* the chain hash after this batch */
  "  root BLOB"                    /* the state root after it, when known at the finalize (page mode) */
  ");"
  /* the part of a transaction a replica holds back until its end is posted (#57) */
  "CREATE TABLE IF NOT EXISTS _hpplite_carry("
  "  seq INTEGER PRIMARY KEY,"
  "  sql TEXT NOT NULL,"
  "  mode INTEGER NOT NULL DEFAULT 0,"
  "  now INTEGER NOT NULL DEFAULT 0,"
  "  frag INTEGER NOT NULL DEFAULT 0"
  ");"
  /* the readers batches are sealed to besides the wallet (#15): compressed public keys */
  "CREATE TABLE IF NOT EXISTS _hpplite_readers("
  "  pubkey BLOB PRIMARY KEY,"
  "  added_height INTEGER NOT NULL DEFAULT 0"
  ") WITHOUT ROWID;"
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
** (that silently dropped captures, issue #20). hppliteBkEnter() sets the flag only
** when bookkeeping shares the application connection (:memory:).
*/
int hppliteBkEnter(HppliteCtx *pCtx) {
  int saved = pCtx->bInternal;
  if (pCtx->bkDb == pCtx->db) pCtx->bInternal = 1;
  return saved;
}
void hppliteBkLeave(HppliteCtx *pCtx, int saved) {
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
/*
** A follower's copy of the meta row is from its open; the sequencer moves
** the file. Before reporting (hpplite_status, the height API) a follower
** looks at the file's height and reloads when it moved (#100). One row.
*/
void hpplite_refresh_meta(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int saved;
  if (!pCtx || !pCtx->bkDb) return;
  saved = hppliteBkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT next_height FROM _hpplite_meta WHERE id=1", -1, &pStmt, NULL) == SQLITE_OK) {
    int moved = sqlite3_step(pStmt) == SQLITE_ROW && (uint64_t)sqlite3_column_int64(pStmt, 0) != pCtx->blockHeight;
    sqlite3_finalize(pStmt);
    if (moved) { hppliteBkLeave(pCtx, saved); hppliteLoadMeta(pCtx); return; }
  }
  hppliteBkLeave(pCtx, saved);
}

int hppliteLoadMeta(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int rc;
  int saved = hppliteBkEnter(pCtx);
  if (pCtx->bPages) {
    sqlite3_stmt *q = NULL;
    pCtx->inflightHeight = 0;
    if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT inflight_height, inflight_root FROM _hpplite_meta WHERE id = 1", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
      pCtx->inflightHeight = (uint64_t)sqlite3_column_int64(q, 0);
      if (sqlite3_column_bytes(q, 1) == 32) memcpy(pCtx->inflightRoot, sqlite3_column_blob(q, 1), 32); else pCtx->inflightHeight = 0;
    }
    sqlite3_finalize(q);
  }
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT next_height, prev_block_hash, state_root,"
    "       last_checkpoint_height, last_checkpoint_root, open_txn,"
    "       (SELECT count(*) FROM _hpplite_carry), frag_id, frag_off, rollup, chain_hash"
    "  FROM _hpplite_meta WHERE id=1", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) {
      pCtx->blockHeight = (uint64_t)sqlite3_column_int64(pStmt, 0);
      columnHash(pStmt, 1, pCtx->prevBlockHash);
      columnHash(pStmt, 10, pCtx->chainHash);
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

  hppliteBkLeave(pCtx, saved);
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

/* Every table of HPPLITE_SCHEMA, every column added since (#52, #56, #57,
** #59, the rollup column, the tree columns of #9) and the meta row: nothing
** of the schema would write. A missing table makes the query fail to
** prepare, which is "no". Anything added later must be listed here too, or
** a file from before never gets it (#131). */
static int bookkeepingIsCurrent(sqlite3 *db) {
  static const char zSql[] =
    "SELECT (SELECT count(*) FROM pragma_table_info('_hpplite_meta')"
    "          WHERE name IN ('open_txn','frag_id','frag_off','rollup','chain_hash','tree_seq','tree_stale')) = 7"
    "   AND (SELECT count(*) FROM pragma_table_info('_hpplite_pending') WHERE name IN ('mode','now')) = 2"
    "   AND (SELECT count(*) FROM pragma_table_info('_hpplite_carry') WHERE name IN ('now','frag')) = 2"
    "   AND EXISTS(SELECT 1 FROM pragma_table_info('_hpplite_posted'))"
    "   AND EXISTS(SELECT 1 FROM pragma_table_info('_hpplite_tree'))"
    "   AND EXISTS(SELECT 1 FROM pragma_table_info('_hpplite_readers'))"
    "   AND EXISTS(SELECT 1 FROM _hpplite_meta WHERE id = 1)";
  sqlite3_stmt *q = NULL;
  int ok = 0;
  if (sqlite3_prepare_v2(db, zSql, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW)
    ok = sqlite3_column_int(q, 0);
  sqlite3_finalize(q);
  return ok;
}

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
void hpplite_set_init_error(const char *zWhy) { gInitError = zWhy; }   /* a node's init saying why an open fails (page mode, #146) */
const char *hpplite_init_error(void) { return gInitError; }

static int expandedSqlIsFaithful(sqlite3 *db) {
  static const char zLong[] = "hpplite-probe-0123456789abcdefghijklmnopqrstuvwxyz-0123456789abcdefghijklmnopqrstuvwxyz";
  sqlite3_stmt *st = NULL;
  char *z;
  int ok = 0;
#ifdef HPPLITE_TEST_HOOKS
  if (getenv("HPPLITE_TEST_TRUNCATING_EXPAND")) return 0;
#endif
  /* a prepare that fails says nothing about expansion (a file this SQLite cannot
  ** read, #139): the schema step reports that; truncation is only what is seen */
  if (sqlite3_prepare_v2(db, "SELECT ?1", -1, &st, NULL) != SQLITE_OK) return 1;
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

  /* A file whose bookkeeping exists and is current is opened with reads
  ** alone (#123): CREATE IF NOT EXISTS of a table that exists reads, but the
  ** INSERT OR IGNORE of the meta row takes the write lock even when the row
  ** is there, and a connection opened while another holds a write
  ** transaction (one connection per request) waited out the busy timeout
  ** and started without HPPLite. The first open of a file, and an upgrade
  ** of an older file, still write. */
  rc = SQLITE_OK;
  if (!bookkeepingIsCurrent(db)) {
    rc = hppliteInternalExec(pCtx, db, HPPLITE_SCHEMA);
    if (rc != SQLITE_OK) {
      /* a file this SQLite cannot read: not a database, or encrypted at the page
      ** level (SQLCipher, SEE) and not keyed before HPPLite started (#139) */
      static char zWhy[300];
      snprintf(zWhy, sizeof zWhy, "cannot create the bookkeeping tables: %s (the file is not a database, or it is encrypted"
               " and not yet keyed: see issue #139)", sqlite3_errmsg(db));
      gInitError = zWhy;
    }
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
    if (rc == SQLITE_OK) {
      /* #33: the rollup's rolling hash; a file from before has none (NULL), which no check reads */
      sqlite3_stmt *q = NULL;
      int has = 0;
      if (sqlite3_prepare_v2(db, "SELECT 1 FROM pragma_table_info('_hpplite_meta') WHERE name = 'chain_hash'", -1, &q, NULL) == SQLITE_OK)
        has = (sqlite3_step(q) == SQLITE_ROW);
      sqlite3_finalize(q);
      if (!has) rc = hppliteInternalExec(pCtx, db, "ALTER TABLE _hpplite_meta ADD COLUMN chain_hash BLOB");
    }
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
    /* #9: the maintained state trees */
    if (rc == SQLITE_OK) rc = hppliteTreeSchema(pCtx, db);
  }
  if (rc != SQLITE_OK) {
    sqlite3_free(pCtx);
    return rc;
  }
  hppliteLoadMeta(pCtx);
  pCtx->vacuumFd = -1;
  {
    /* the VACUUM barrier (#75): one lock file per database file, by its real path */
    const char *zFile = sqlite3_db_filename(db, "main");
    char resolved[PATH_MAX], path[PATH_MAX + 24];
    if (zFile && zFile[0] && realpath(zFile, resolved)) {
      snprintf(path, sizeof path, "%s-hpplite-vacuum", resolved);
      pCtx->vacuumFd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);     /* -1 (a read-only directory): no barrier */
    }
  }
  pCtx->seenSchema = hppliteSchemaVersion(pCtx);
  pCtx->bClockPin = hpplite_clock_on(db);
  hpplitePragmaModeRead(pCtx);

  sqlite3_commit_hook(db, hppliteCommitHook, pCtx);
  sqlite3_rollback_hook(db, hppliteRollbackHook, pCtx);
  sqlite3_trace_v2(db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE, hppliteTraceCallback, pCtx);
  sqlite3_set_authorizer(db, hppliteAuthorizer, pCtx);
  hppliteBlobGuardArm(pCtx, 1);

  *ppCtx = pCtx;
  return SQLITE_OK;
}

static int pagesTrace(unsigned mask, void *pArg, void *pS, void *pX);
static int pagesWalHook(void *pArg, sqlite3 *db, const char *zDb, int nFrames);
static void rearmHooks(HppliteCtx *pCtx, int bClosing) {
  void *prevCommit, *prevRollback;
  int lost = 0;
  if (!pCtx || pCtx->bDbClosed) return;
  if (pCtx->bPages) {   /* page mode: the authorizer, the VACUUM marker (#148) and the wal hook (#155) */
    sqlite3_set_authorizer(pCtx->db, hppliteAuthorizer, pCtx);
    sqlite3_trace_v2(pCtx->db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE, pagesTrace, pCtx);
    sqlite3_wal_hook(pCtx->db, pagesWalHook, pCtx);
    return;
  }
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
  hppliteBlobGuardArm(pCtx, 0);
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
  if (pCtx->pProbeStmt) sqlite3_finalize(pCtx->pProbeStmt);
  pCtx->pLogStmt = pCtx->pUnlogStmt = pCtx->pSchemaStmt = pCtx->pRecTrigStmt = pCtx->pZeroStmt = NULL;
  pCtx->pSeqStmt = pCtx->pRowsSchemaStmt = pCtx->pRowsTempSchemaStmt = pCtx->pRowsLockStmt = pCtx->pProbeStmt = NULL;
  pCtx->logged.pS = NULL;
}

/* Statements of the application that are still open (ours do not count).
** Application thread only: it walks the connection. */
static int applicationStatementsOpen(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt = NULL;
  while ((pStmt = sqlite3_next_stmt(pCtx->db, pStmt)) != NULL) {
    if (pStmt == pCtx->pLogStmt || pStmt == pCtx->pUnlogStmt || pStmt == pCtx->pSchemaStmt
        || pStmt == pCtx->pRecTrigStmt || pStmt == pCtx->pZeroStmt || pStmt == pCtx->pSeqStmt
        || pStmt == pCtx->pRowsSchemaStmt || pStmt == pCtx->pRowsTempSchemaStmt || pStmt == pCtx->pRowsLockStmt
        || pStmt == pCtx->pProbeStmt) continue;
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
    if (pCtx->bPages) sqlite3_wal_hook(pCtx->db, NULL, NULL);
    hpplite_rows_disable(pCtx);           /* its triggers and functions hold pCtx (#66) */
    finalizeOwnStatements(pCtx);
  }
  sqlite3_free(pCtx->zWalKey); pCtx->zWalKey = NULL;
  hppliteFreeFunctionChecks(pCtx);
  hppliteTreeFree(pCtx);
  if (pCtx->vacuumFd >= 0) close(pCtx->vacuumFd);                         /* its flock goes with it */
  sqlite3_free(pCtx->aNoClock);
  sqlite3_free(pCtx->zRowsTextOnly);
  sqlite3_free(pCtx->zRowsSeq);
  sqlite3_free(pCtx->zDbName);
  if (pCtx->pNoClockMutex) sqlite3_mutex_free(pCtx->pNoClockMutex);
  sqlite3_free(pCtx);
}

/* Page mode's trace: VACUUM attaches a database to copy through, which the
** authorizer lets pass while a VACUUM runs (row mode marks it from its own
** trace); nothing else is read from the statements (#148) */
static int pagesTrace(unsigned mask, void *pArg, void *pS, void *pX) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  if (!pCtx) return 0;
  if (mask == SQLITE_TRACE_STMT) {
    const char *z = (const char*)pX;
    /* the wal hook, ours again: sqlite3_open sets SQLite's auto-checkpoint hook
    ** after the auto-extensions ran, and a driver may set it later (#155) */
    if (sqlite3_wal_hook(pCtx->db, pagesWalHook, pCtx) != pCtx) { /* replaced since: ours now */ }
    pCtx->bWalHookFired = 0;
    while (z && (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r')) z++;
    if (z && sqlite3_strnicmp(z, "VACUUM", 6) == 0 && !isalnum((unsigned char)z[6]) && z[6] != '_') pCtx->pVacuumStmt = pS;
  } else if (mask == SQLITE_TRACE_PROFILE) {
    if (pS == pCtx->pVacuumStmt) pCtx->pVacuumStmt = NULL;
    /* SQLite runs the wal hook only when sqlite3_step() returns SQLITE_DONE
    ** in autocommit (vdbeapi.c, doWalCallbacks): a statement reset after a
    ** row - RETURNING stepped once, a driver's .get() - commits at the reset
    ** without it. The statement's end reads whatever was committed since */
    if (!pCtx->bWalHookFired && pCtx->zWalKey && sqlite3_get_autocommit(pCtx->db) && !sqlite3_stmt_readonly((sqlite3_stmt *)pS))
      hpplite_walread_poll(pCtx->zWalKey, 0);                   /* only a statement that can have committed: not one inside a transaction, not a read */
  }
  return 0;
}

/* Page mode's wal hook (#155): after each commit of this connection the WAL
** reader takes the commit's frames, on this thread, with the count SQLite
** hands over; and the checkpoint SQLite's own hook would have run at 1000
** frames is this hook's now (the node's maintenance when it has pins, a
** passive checkpoint otherwise) */
static int pagesWalHook(void *pArg, sqlite3 *db, const char *zDb, int nFrames) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  (void)zDb;
  if (!pCtx) return SQLITE_OK;
  pCtx->bWalHookFired = 1;
  if (pCtx->zWalKey) hpplite_walread_poll(pCtx->zWalKey, (unsigned)nFrames);
  if (nFrames >= 1000) {
    if (pCtx->xWalMaint) pCtx->xWalMaint(pCtx->pWalMaintArg);
    else sqlite3_wal_checkpoint_v2(db, NULL, SQLITE_CHECKPOINT_PASSIVE, NULL, NULL);
  }
  return SQLITE_OK;
}

int hpplite_init_pages(sqlite3 *db, sqlite3 *bkDb, HppliteCtx **ppCtx) {
  HppliteCtx *pCtx = (HppliteCtx*)sqlite3_malloc(sizeof(HppliteCtx));
  int rc = SQLITE_OK;
  if (!pCtx) return SQLITE_NOMEM;
  memset(pCtx, 0, sizeof(HppliteCtx));
  gInitError = NULL;
  pCtx->seenSchema = -1;
  pCtx->db = db;
  pCtx->bkDb = bkDb;
  pCtx->bEnabled = 0;
  pCtx->bPages = 1;
  pCtx->vacuumFd = -1;
  {
    /* a file this SQLite cannot read (not a database; SQLCipher/SEE and not
    ** keyed, #139): row capture finds out creating its tables in it, page
    ** mode keeps nothing in the file, so it asks (#149) */
    sqlite3_stmt *q = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT count(*) FROM sqlite_schema", -1, &q, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_step(q) == SQLITE_ROW ? SQLITE_OK : sqlite3_errcode(db);
    sqlite3_finalize(q);
    if (rc != SQLITE_OK) {
      static char zWhy[300];
      snprintf(zWhy, sizeof zWhy, "cannot read the database: %s (the file is not a database, or it is encrypted"
               " and not yet keyed: see issue #139)", sqlite3_errmsg(db));
      gInitError = zWhy;
      sqlite3_free(pCtx);
      return rc;
    }
  }
  if (!bookkeepingIsCurrent(bkDb)) {
    rc = hppliteInternalExec(pCtx, bkDb, HPPLITE_SCHEMA);
    if (rc == SQLITE_OK) rc = hppliteTreeSchema(pCtx, bkDb);
    if (rc != SQLITE_OK) { gInitError = "cannot create the bookkeeping tables in the sidecar"; sqlite3_free(pCtx); return rc; }
  }
  {
    /* the in-flight batch (a submit the process may not survive) */
    sqlite3_stmt *q = NULL;
    int has = 0;
    if (sqlite3_prepare_v2(bkDb, "SELECT 1 FROM pragma_table_info('_hpplite_meta') WHERE name = 'inflight_height'", -1, &q, NULL) == SQLITE_OK) has = (sqlite3_step(q) == SQLITE_ROW);
    sqlite3_finalize(q);
    if (!has) hppliteInternalExec(pCtx, bkDb, "ALTER TABLE _hpplite_meta ADD COLUMN inflight_height INTEGER NOT NULL DEFAULT 0; ALTER TABLE _hpplite_meta ADD COLUMN inflight_root BLOB");
  }
  hppliteLoadMeta(pCtx);
  pCtx->bRootDirty = 0;
  /* no commit or trace hook (the VFS sees the commits), but the rules that keep
  ** the capture possible stand: the file stays in WAL (the frames are what is
  ** watched), ATTACH is refused (another file is not captured), the prefix is
  ** reserved (#148) */
  sqlite3_set_authorizer(db, hppliteAuthorizer, pCtx);
  sqlite3_trace_v2(db, SQLITE_TRACE_STMT | SQLITE_TRACE_PROFILE, pagesTrace, pCtx);
  {
    /* the WAL reader's key: the file's canonical path (the node resolves the
    ** same for its page mode) */
    const char *zFile = sqlite3_db_filename(db, "main");
    char resolved[PATH_MAX];
    if (zFile && zFile[0]) pCtx->zWalKey = sqlite3_mprintf("%s", realpath(zFile, resolved) ? resolved : zFile);
  }
  sqlite3_wal_hook(db, pagesWalHook, pCtx);
  *ppCtx = pCtx;
  return SQLITE_OK;
}

int hpplite_set_bookkeeping_db(HppliteCtx *pCtx, sqlite3 *bkDb) {
  if (!pCtx) return SQLITE_MISUSE;
  pCtx->bkDb = bkDb ? bkDb : pCtx->db;
  return hppliteLoadMeta(pCtx);
}

void hpplite_enable(HppliteCtx *pCtx, int bEnable) {
  if (pCtx) pCtx->bEnabled = bEnable;
}

void hpplite_get_state_root(HppliteCtx *pCtx, unsigned char *pOut) {
  if (!pCtx || !pOut) return;
  if (pCtx->bPages) { memcpy(pOut, pCtx->stateRoot, HPPLITE_HASH_SIZE); return; }   /* the page root, kept by the node (#146) */
  if (pCtx->bRootDirty) {
    /* the trees take the log in; a write transaction keeps what they took,
    ** a read one computes the root and keeps the work in memory for the
    ** next flush; on the shared connection inside the application's
    ** transaction, neither: the statements run as they can */
    int txn = hppliteBkBegin(pCtx, 1) == SQLITE_OK ? 1 : (hppliteBkBegin(pCtx, 0) == SQLITE_OK ? 0 : -1);
    int saved = hppliteBkEnter(pCtx);
    int rc = hppliteTreeRoot(pCtx, pCtx->curRoot);
    if (rc == SQLITE_OK && txn == 1) rc = hppliteTreeFlush(pCtx);
    hppliteBkLeave(pCtx, saved);
    if (txn >= 0) rc = hppliteBkEnd(pCtx, rc);
    if (rc == SQLITE_OK) pCtx->bRootDirty = 0;
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

int hpplite_pending_count(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt;
  int n = 0;
  if (!pCtx) return 0;
  int saved = hppliteBkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT count(*) FROM _hpplite_pending",
                         -1, &pStmt, NULL) == SQLITE_OK) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) n = sqlite3_column_int(pStmt, 0);
    sqlite3_finalize(pStmt);
  }
  hppliteBkLeave(pCtx, saved);
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

/*
** ============================================================================
** Simple Open API (M3)
** ============================================================================
*/

#include "config.h"
#include "keystore.h"
#include "envelope_pk.h"
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

HppliteNode *hppliteFindNode(sqlite3 *db) {
    NodeEntry *e = findEntry(db);
    return e ? e->node : NULL;
}

void hppliteRemoveNode(sqlite3 *db) {
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
static void hpplite_anchor_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_key_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_keyfile_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_keystore_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_batch_key_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_set_sequencer_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_grant_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_revoke_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);
static void hpplite_viewkey_func(sqlite3_context *ctx, int argc, sqlite3_value **argv);


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
    sqlite3_create_function(db, "hpplite_anchor", 0, SQLITE_UTF8, NULL,
                            hpplite_anchor_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_key", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL,
                            hpplite_key_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_keyfile", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL,
                            hpplite_keyfile_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_keystore", 2, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL,
                            hpplite_keystore_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_batch_key", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL,
                            hpplite_batch_key_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_set_sequencer", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL,
                            hpplite_set_sequencer_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_grant", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, hpplite_grant_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_revoke", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, hpplite_revoke_func, NULL, NULL);
    sqlite3_create_function(db, "hpplite_viewkey", 1, SQLITE_UTF8 | SQLITE_DIRECTONLY, NULL, hpplite_viewkey_func, NULL, NULL);
    hppliteStatusRegister(db);
}

/*
** Connection close. Runs from the hpplite_status virtual table's xDisconnect,
** which SQLite invokes at the start of sqlite3_close() / sqlite3_close_v2()
** while the connection is still fully usable - the same point a close hook
** in core would give us, without patching core.
*/
void hppliteOnClose(sqlite3 *db) {
    HppliteNode *node = hppliteFindNode(db);
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
    hppliteRemoveNode(db);

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
    hppliteRemoveNode(db);
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
    memcpy(nc->viewkey, cfg->viewkey, 32);
    nc->hasViewKey = cfg->hasViewKey;
    memcpy(nc->factory, cfg->factory, 20);
    nc->hasFactory = cfg->hasFactory;
    memcpy(nc->contract, cfg->contract, 20);
    nc->hasContract = cfg->hasContract;
    nc->chainId = cfg->chainId;
    nc->batchIntervalMs = cfg->batchIntervalMs;
    nc->noEncrypt = !cfg->encrypt;
    nc->noCompress = !cfg->compress;
    nc->determinismWarn = cfg->determinismWarn;
    nc->captureText = cfg->captureText;
    nc->capturePages = cfg->capturePages;
    nc->captureSource = cfg->captureSource;
    nc->captureAuto = cfg->captureAuto;
    nc->localOnly = cfg->localOnly;
    nc->verifyOnly = cfg->role == HPPLITE_CFG_ROLE_VERIFY;
    nc->dbName = cfg->dbName;
    nc->maxTxBytes = cfg->maxTxBytes;
    nc->maxTxGas = cfg->maxTxGas;
    nc->checkpointInterval = cfg->checkpointInterval;
    nc->storeUrl = cfg->storeUrl;
    nc->storeCap = cfg->storeCap;
    nc->storeFallback = cfg->storeFallback;
    nc->anchorIntervalMs = cfg->anchorIntervalMs;   /* the anchor's cadence (#164) */

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
    HppliteNode *node = hppliteFindNode(db);
    return node ? node->ctx : NULL;
}

struct HppliteConfig *hpplite_get_config(sqlite3 *db) {
    NodeEntry *e = findEntry(db);
    return e ? e->config : NULL;
}

HppliteL1 *hpplite_get_l1(sqlite3 *db) {
    HppliteNode *node = hppliteFindNode(db);
    return node ? node->l1 : NULL;
}

HppliteNode *hpplite_get_node(sqlite3 *db) {
    return hppliteFindNode(db);
}

uint64_t hpplite_flush(sqlite3 *db) {
    HppliteNode *node = hppliteFindNode(db);
    if (!node) return 0;
    /* Use the node's flush (sequencer path) */
    return hpplite_node_flush_batch(node);
}

uint64_t hpplite_anchor(sqlite3 *db) {
    HppliteNode *node = hppliteFindNode(db);
    if (!node) return 0;
    return hpplite_node_anchor(node);
}

static void hpplite_anchor_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    (void)argc; (void)argv;
    sqlite3_result_int64(ctx, (sqlite3_int64)hpplite_anchor(sqlite3_context_db_handle(ctx)));
}

/*
** Check if auto-flush is needed and perform it if so.
** Returns batch height if flushed, 0 otherwise.
** Note: With timer thread, this is less necessary but kept for compatibility.
*/
uint64_t hpplite_check_flush(sqlite3 *db) {
    HppliteNode *node = hppliteFindNode(db);
    if (!node) return 0;
    /* The timer thread handles auto-flush; this is a manual trigger */
    return hpplite_node_flush_batch(node);
}

void hpplite_state_root(sqlite3 *db, unsigned char *out) {
    HppliteNode *node = hppliteFindNode(db);
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
    HppliteNode *node = hppliteFindNode(db);

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
/* SELECT hpplite_key('0x...') / hpplite_keyfile('/path') (#17): the key given
** to the connection after open, the SQLite-extension idiom - nothing is in
** the URI. 1 when the node is keyed; an error with the reason otherwise. */
static void keyGiven(sqlite3_context *ctx, const HppliteConfig *cfg, const char *what) {
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = hppliteFindNode(db);
    int rc;
    if (!node) { sqlite3_result_error(ctx, "hpplite is not on this connection", -1); return; }
    rc = hpplite_node_set_key(node, cfg->privkey);
    if (rc == 0) sqlite3_result_int(ctx, 1);
    else if (rc == -2) sqlite3_result_error(ctx, "this connection is keyed already with another key", -1);
    else {
        char *z = sqlite3_mprintf("%s: %s", what, node->ctx && node->ctx->zLastError[0] ? node->ctx->zLastError : "not a usable key");
        sqlite3_result_error(ctx, z ? z : what, -1);
        sqlite3_free(z);
    }
}
static void hpplite_key_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    HppliteConfig cfg;
    const char *z = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    memset(&cfg, 0, sizeof cfg);
    if (!z || hpplite_config_parse_privkey(&cfg, z) != 0) { sqlite3_result_error(ctx, "hpplite_key: not a 32-byte hex key", -1); return; }
    keyGiven(ctx, &cfg, "hpplite_key");
    memset(&cfg, 0, sizeof cfg);
}
/* SELECT hpplite_grant('0x<33-byte compressed public key>') / hpplite_revoke (#15): the
** readers the next batch series is sealed to, besides the wallet. A grant covers what is
** posted from here on; history stays as it was sealed. 1, or an error. */
static void readerChange(sqlite3_context *ctx, int argc, sqlite3_value **argv, int bGrant) {
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = hppliteFindNode(db);
    const char *z = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    unsigned char pub[33];
    char *sql, hex[67];
    int rc;
    if (!node || !node->ctx) { sqlite3_result_error(ctx, "hpplite is not on this connection", -1); return; }
    if (!z || hpplite_hex_to_bytes(z, pub, 33) != 33 || (pub[0] != 2 && pub[0] != 3)) {
        sqlite3_result_error(ctx, "a reader is a 33-byte compressed secp256k1 public key in hex (02... or 03...)", -1);
        return;
    }
    hpplite_bytes_to_hex(pub, 33, hex);
    sql = sqlite3_mprintf(bGrant ? "INSERT OR IGNORE INTO _hpplite_readers(pubkey, added_height) VALUES(x'%s', %llu)"
                                 : "DELETE FROM _hpplite_readers WHERE pubkey = x'%s'",
                          hex, (unsigned long long)hpplite_get_block_height(node->ctx));
    if (!sql) { sqlite3_result_error_nomem(ctx); return; }
    rc = hppliteInternalExec(node->ctx, node->ctx->bPages ? node->ctx->bkDb : db, sql);   /* page mode: the readers live in the sidecar (#150) */
    sqlite3_free(sql);
    if (rc != SQLITE_OK) { sqlite3_result_error(ctx, bGrant ? "hpplite_grant: not recorded" : "hpplite_revoke: not recorded", -1); return; }
    hpplite_node_readers_changed(node);
    sqlite3_result_int(ctx, 1);
}
static void hpplite_grant_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) { readerChange(ctx, argc, argv, 1); }
static void hpplite_revoke_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) { readerChange(ctx, argc, argv, 0); }

/* SELECT hpplite_viewkey('0x...') (#15): a viewing key given after open */
static void hpplite_viewkey_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = hppliteFindNode(db);
    const char *z = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    unsigned char key[32];
    if (!node) { sqlite3_result_error(ctx, "hpplite is not on this connection", -1); return; }
    if (!z || hpplite_hex_to_bytes(z, key, 32) != 32 || hpplite_node_set_view_key(node, key) != 0) {
        sqlite3_result_error(ctx, "hpplite_viewkey: not a 32-byte secp256k1 private key in hex", -1);
        return;
    }
    memset(key, 0, sizeof key);
    sqlite3_result_int(ctx, 1);
}

/* SELECT hpplite_set_sequencer('0x...') (#17): the owner names the wallet that may hold
** the lease and post, the hot key apart from the owner's; the running lease expires.
** The transaction's hash. Only the owner's transaction goes through. */
static void hpplite_set_sequencer_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = hppliteFindNode(db);
    const char *z = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    unsigned char wallet[20];
    char *tx;
    if (!node) { sqlite3_result_error(ctx, "hpplite is not on this connection", -1); return; }
    if (!node->l1) { sqlite3_result_error(ctx, "hpplite_set_sequencer: no chain on this connection", -1); return; }
    if (!z || hpplite_hex_to_bytes(z, wallet, 20) != 20) { sqlite3_result_error(ctx, "hpplite_set_sequencer: not a 20-byte hex address", -1); return; }
    tx = hpplite_l1_set_sequencer(node->l1, wallet);
    if (!tx) { sqlite3_result_error(ctx, "hpplite_set_sequencer: the chain did not take it (only the rollup's owner may; a wallet of zero is refused)", -1); return; }
    hpplite_l1_wait_for_tx(node->l1, tx, 60);                  /* mined before anyone reads the lease again */
    if (node->hasLease) { node->hasLease = 0; node->leaseRecheckAt = 0; }   /* ours expired with the rotation, unless we named ourselves */
    sqlite3_result_text(ctx, tx, -1, SQLITE_TRANSIENT);
    sqlite3_free(tx);
}

/* SELECT hpplite_batch_key('0x...') (#17): the 256-bit batch cipher key as is, for a node
** whose sequencer key lives in a signer and derives nothing */
static void hpplite_batch_key_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = hppliteFindNode(db);
    const char *z = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    unsigned char key[32];
    if (!node) { sqlite3_result_error(ctx, "hpplite is not on this connection", -1); return; }
    if (!z || hpplite_hex_to_bytes(z, key, 32) != 32) { sqlite3_result_error(ctx, "hpplite_batch_key: not 32 bytes of hex", -1); return; }
    if (hpplite_node_set_batch_key(node, key) != 0) { sqlite3_result_error(ctx, "hpplite_batch_key: not set", -1); return; }
    memset(key, 0, sizeof key);
    sqlite3_result_int(ctx, 1);
}

/* SELECT hpplite_keystore('/path.json', 'passphrase') (#17): an Ethereum keystore V3 file */
static void hpplite_keystore_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    HppliteConfig cfg;
    char err[200];
    const char *z = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    const char *pw = argc > 1 ? (const char *)sqlite3_value_text(argv[1]) : NULL;
    memset(&cfg, 0, sizeof cfg);
    if (!z || hpplite_keystore_load(z, pw, cfg.privkey, err, sizeof err) != 0) {
        char *msg = sqlite3_mprintf("hpplite_keystore: %s", z ? err : "no path");
        sqlite3_result_error(ctx, msg ? msg : "hpplite_keystore", -1);
        sqlite3_free(msg);
        return;
    }
    cfg.hasPrivkey = 1;
    keyGiven(ctx, &cfg, "hpplite_keystore");
    memset(&cfg, 0, sizeof cfg);
}
static void hpplite_keyfile_func(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
    HppliteConfig cfg;
    const char *z = argc > 0 ? (const char *)sqlite3_value_text(argv[0]) : NULL;
    memset(&cfg, 0, sizeof cfg);
    if (!z || hpplite_config_load_privkey_file(&cfg, z) != 0) { sqlite3_result_error(ctx, "hpplite_keyfile: the file is not a usable key (missing, readable by others, or not a 32-byte hex key)", -1); return; }
    keyGiven(ctx, &cfg, "hpplite_keyfile");
    memset(&cfg, 0, sizeof cfg);
}

static void hpplite_flush_func(
    sqlite3_context *ctx,
    int argc,
    sqlite3_value **argv
) {
    (void)argc;
    (void)argv;

    sqlite3 *db = sqlite3_context_db_handle(ctx);
    HppliteNode *node = hppliteFindNode(db);

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
    if (dup && hppliteFindNode(db)) return SQLITE_OK;
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
