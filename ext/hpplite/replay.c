/*
** HPPLite - replay: applying batches fetched from the chain.
**
** A replica rebuilds the file from the batches alone: each statement or row
** record runs under the connection state it was captured in (#52), the part
** of a transaction whose end is not posted yet is carried (#57), a
** statement posted in pieces is reassembled (#59), and the posted root is
** compared with the content hash. Adopting a batch is the writer's own
** replay: the batch on the chain was built from its pending rows.
*/

#include "hpplite_int.h"
#include "state_tree.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
static void runEntry(void *pArg, int i, const char **pzSql, int *pMode) {
  const ReplayStmt *a = (const ReplayStmt *)pArg;
  *pzSql = a[i].zSql; *pMode = a[i].mode;
}

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


/* The verifier's ring of rebuilt roots (#120) */
static void rootAtRecord(HppliteCtx *pCtx, uint64_t height, const unsigned char root[32]) {
  int i = pCtx->nRootAt % HPPLITE_ROOT_RING;
  pCtx->aRootAt[i].height = height;
  memcpy(pCtx->aRootAt[i].root, root, 32);
  pCtx->nRootAt++;
}
int hpplite_root_at(HppliteCtx *pCtx, uint64_t height, unsigned char out[32]) {
  int i, n;
  if (!pCtx) return -1;
  n = pCtx->nRootAt < HPPLITE_ROOT_RING ? pCtx->nRootAt : HPPLITE_ROOT_RING;
  for (i = 0; i < n; i++) {
    if (pCtx->aRootAt[i].height == height) { memcpy(out, pCtx->aRootAt[i].root, 32); return 0; }
  }
  if (pCtx->rootCmpHeight == height && height > 0) { memcpy(out, pCtx->rootCmp, 32); return 0; }
  return -1;
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
    int saved = hppliteBkEnter(pCtx);
    nCarry = loadCarry(pCtx, &azCarry, &aCarryMode, &aCarryNow, &aCarryFrag);
    hppliteBkLeave(pCtx, saved);
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
    rc = hppliteBkBegin(pCtx, 1);
    if (rc == SQLITE_OK) {
      int saved = hppliteBkEnter(pCtx);
      /* the held part moves with the height, in one transaction */
      if (carryChanged) rc = storeCarry(pCtx, aRun + holdFrom, nHold);
      if (rc == SQLITE_OK) rc = hppliteTreeTouchEntries(pCtx, holdFrom, runEntry, aRun);   /* what ran, into the state trees (#9) */
      if (rc == SQLITE_OK) rc = hppliteAdvanceMeta(pCtx, pBatch, 0, 0);
      hppliteBkLeave(pCtx, saved);
      rc = hppliteBkEnd(pCtx, rc);
      if (rc == SQLITE_OK) pCtx->nCarry = nHold;
      else hppliteLoadMeta(pCtx);              /* memory follows the file (#97) */
    }
  }
  for (i = 0; i < nCarry; i++) sqlite3_free(azCarry[i]);
  sqlite3_free(azCarry); sqlite3_free(aCarryMode); sqlite3_free(aCarryNow); sqlite3_free(aCarryFrag);
    for (i = 0; i < nJoined; i++) sqlite3_free(azJoined[i]);
    sqlite3_free(azJoined);
  sqlite3_free(aRun);
  if (rc != SQLITE_OK) return rc;

  /* The seed of `hpplite verify`: does the content match what was posted?
  ** Only batches that carry a root (checkpoint boundaries) can be checked.
  ** A verifier also keeps the rebuilt root after every recent batch, for an
  ** on-chain checkpoint at a height with no posted root (#120). */
  if (!pBatch->hasRoot) {
    if (pCtx->bTrackRoots) {
      hpplite_get_state_root(pCtx, root);
      rootAtRecord(pCtx, pBatch->height, root);
    }
    return SQLITE_OK;
  }
  if (pBatch->rootVersion < 2) {
    /* a history from before #9: its roots are one hash over every row */
    int saved = hppliteBkEnter(pCtx);
    if (hpplite_db_hash_v1(pCtx->bkDb, root) != SQLITE_OK) memset(root, 0, sizeof root);
    hppliteBkLeave(pCtx, saved);
  } else {
    hpplite_get_state_root(pCtx, root);
  }
  pCtx->nRootsCompared++;
  pCtx->rootCmpHeight = pBatch->height;        /* for the verifier: compared with the on-chain checkpoint (#5) */
  memcpy(pCtx->rootCmp, root, HPPLITE_HASH_SIZE);
  if (pCtx->bTrackRoots) rootAtRecord(pCtx, pBatch->height, root);
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

  int saved = hppliteBkEnter(pCtx);
  /* row records (#19) compare as the entry text the batch was built with */
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, sql, mode, CASE WHEN txn = 0 THEN id ELSE txn END, now, op, tbl, rec FROM _hpplite_pending ORDER BY id LIMIT ?1",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) rc = sqlite3_prepare_v2(pCtx->bkDb,
    "SELECT id, sql, mode, CASE WHEN txn = 0 THEN id ELSE txn END, now FROM _hpplite_pending ORDER BY id LIMIT ?1",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) { hppliteBkLeave(pCtx, saved); return rc; }
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
  hppliteBkLeave(pCtx, saved);
  if (!ok || i != pBatch->nTxns) return SQLITE_MISMATCH;

  /* what it left open is the group of its last statement (#57) */
  adopted = *pBatch;
  adopted.openGroup = pBatch->openTxn ? lastGroup : 0;
  adopted.fragRow = fragRow;
  adopted.fragOff = fragOff;
  return hpplite_finalize_batch(pCtx, &adopted, maxId);
}

/* Step past a height whose batch fails authentication (posted under a lease we do
** not hold): every key holder skips it the same way, prev_block_hash becoming the
** hash of the raw bytes; and persist checkpoint progress (hpplite.h) */
int hpplite_skip_height(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRawHash, const unsigned char *pRawKeccak) {
  sqlite3_stmt *pStmt;
  unsigned char chainHash[HPPLITE_HASH_SIZE];
  int rc;
  if (!pCtx || !pRawHash) return SQLITE_MISUSE;
  if (height != pCtx->blockHeight) return SQLITE_MISMATCH;
  /* the contract committed to those bytes too: the rolling hash steps over them (#33) */
  if (pRawKeccak) hpplite_chain_hash_next(pCtx->chainHash, height, pRawKeccak, chainHash);
  else memcpy(chainHash, pCtx->chainHash, HPPLITE_HASH_SIZE);
  rc = hppliteBkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  {
    int saved = hppliteBkEnter(pCtx);
    rc = sqlite3_prepare_v2(pCtx->bkDb,
      "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, chain_hash=?3 WHERE id=1", -1, &pStmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)(height + 1));
      sqlite3_bind_blob(pStmt, 2, pRawHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
      sqlite3_bind_blob(pStmt, 3, chainHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
      rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
      sqlite3_finalize(pStmt);
    }
    hppliteBkLeave(pCtx, saved);
  }
  rc = hppliteBkEnd(pCtx, rc);
  if (rc == SQLITE_OK) {
    pCtx->blockHeight = height + 1;
    memcpy(pCtx->prevBlockHash, pRawHash, HPPLITE_HASH_SIZE);
    memcpy(pCtx->chainHash, chainHash, HPPLITE_HASH_SIZE);
    pCtx->nForeignBatches++;
  }
  return rc;
}

int hpplite_set_checkpoint(HppliteCtx *pCtx, uint64_t height, const unsigned char *pRoot) {
  sqlite3_stmt *pStmt;
  int rc;
  if (!pCtx || !pRoot) return SQLITE_MISUSE;
  int saved = hppliteBkEnter(pCtx);
  rc = sqlite3_prepare_v2(pCtx->bkDb,
    "UPDATE _hpplite_meta SET last_checkpoint_height=?1, last_checkpoint_root=?2 WHERE id=1",
    -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height);
    sqlite3_bind_blob(pStmt, 2, pRoot, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_finalize(pStmt);
  }
  hppliteBkLeave(pCtx, saved);
  if (rc == SQLITE_OK) {
    pCtx->lastCheckpointHeight = height;
    memcpy(pCtx->lastCheckpointRoot, pRoot, HPPLITE_HASH_SIZE);
  }
  return rc;
}
