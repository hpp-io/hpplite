/*
** HPPLite - building and finalizing batches.
**
** A batch is built from the committed pending log on the bookkeeping
** connection, cut to the DA's size at a transaction boundary (or inside a
** statement, in pieces, #59), given the content-hash root when the node asks
** for one; once posted, finalizing it deletes its rows and advances
** _hpplite_meta in one transaction. batch.c holds the wire format.
*/

#include "hpplite_int.h"
#include "state_tree.h"
#include "crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

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
  if (hppliteBkBegin(pCtx, 1) != SQLITE_OK) return SQLITE_BUSY;
  saved = hppliteBkEnter(pCtx);
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
  hppliteBkLeave(pCtx, saved);
  rc = hppliteBkEnd(pCtx, rc);
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
  if (hppliteBkBegin(pCtx, 0) != SQLITE_OK) return NULL;
  int savedInternal = hppliteBkEnter(pCtx);

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
      if (hppliteTreeRoot(pCtx, root) == SQLITE_OK) {
        hpplite_batch_set_post_state(pBatch, root);
        pBatch->rootVersion = HPPLITE_ROOT_VERSION;
      } else {
        pCtx->nLogErrors++;
        hpplite_error(pCtx, "cannot hash the database for the state root (%s); batch %llu not built",
                sqlite3_errmsg(pCtx->bkDb), (unsigned long long)pBatch->height);
        hpplite_batch_free(pBatch);
        pBatch = NULL;
      }
    }
  }
  hppliteBkLeave(pCtx, savedInternal);
  hppliteBkEnd(pCtx, SQLITE_OK);

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

/* The bytes the pending log would post: statement text, and the row
** records' bytes, which live in the rec column row capture adds (#19) and
** not in sql (#143: counted as nothing, the size-triggered flush never
** fired and a busy writer posted only on the interval). A log from before
** row capture has no rec column: its text alone. */
sqlite3_int64 hpplite_pending_bytes(HppliteCtx *pCtx) {
  sqlite3_stmt *pStmt = NULL;
  sqlite3_int64 n = 0;
  if (!pCtx) return 0;
  int saved = hppliteBkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb,
        "SELECT COALESCE(SUM(length(sql) + COALESCE(length(rec), 0)), 0) FROM _hpplite_pending",
        -1, &pStmt, NULL) != SQLITE_OK) {
    pStmt = NULL;
    sqlite3_prepare_v2(pCtx->bkDb, "SELECT COALESCE(SUM(length(sql)), 0) FROM _hpplite_pending",
                       -1, &pStmt, NULL);
  }
  if (pStmt) {
    if (sqlite3_step(pStmt) == SQLITE_ROW) n = sqlite3_column_int64(pStmt, 0);
    sqlite3_finalize(pStmt);
  }
  hppliteBkLeave(pCtx, saved);
  return n;
}

/*
** Advance _hpplite_meta past pBatch and optionally delete pending rows.
** Caller holds a bookkeeping write transaction.
*/
int hppliteAdvanceMeta(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId, int bWriter) {
  unsigned char blockHash[HPPLITE_HASH_SIZE];
  sqlite3_stmt *pStmt;
  int rc;

  hpplite_block_hash(pBatch, pCtx->prevBlockHash, blockHash);

  /* the state trees take the log in before it is pruned, and are kept (#9):
  ** here only up to this batch's rows; the sync before the transaction took
  ** them in already, and rows that arrived since are the next batch's, taken
  ** in outside the gate (#142) */
  rc = hppliteTreeSyncUpTo(pCtx, maxId);
  if (rc != SQLITE_OK) {
    hpplite_error(pCtx, "the state trees could not take the log in (rc=%d: %s)", rc, sqlite3_errmsg(pCtx->bkDb));
    return rc;
  }
  if (maxId > 0) {
    rc = sqlite3_prepare_v2(pCtx->bkDb,
      "DELETE FROM _hpplite_pending WHERE id <= ?1", -1, &pStmt, NULL);
    if (rc != SQLITE_OK) return rc;
    sqlite3_bind_int64(pStmt, 1, maxId);
    rc = sqlite3_step(pStmt);
    sqlite3_finalize(pStmt);
    if (rc != SQLITE_DONE) return rc;
  }
  rc = hppliteTreeKeep(pCtx, pBatch->hasRoot, maxId);     /* written at the checkpoint, kept in memory between (#142) */
  if (rc != SQLITE_OK) {
    hpplite_error(pCtx, "the state trees could not be kept (rc=%d: %s)", rc, sqlite3_errmsg(pCtx->bkDb));
    return rc;
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

  /* the contract's rolling hash advances by the posted bytes (#33); a batch
  ** with none (l1=local) leaves it where it was */
  unsigned char chainHash[HPPLITE_HASH_SIZE];
  if (pBatch->pPosted && pBatch->nPosted > 0) {
    unsigned char k[HPPLITE_HASH_SIZE];
    hpplite_keccak256(pBatch->pPosted, pBatch->nPosted, k);
    hpplite_chain_hash_next(pCtx->chainHash, pBatch->height, k, chainHash);
  } else {
    memcpy(chainHash, pCtx->chainHash, HPPLITE_HASH_SIZE);
  }

  rc = sqlite3_prepare_v2(pCtx->bkDb,
    pBatch->hasRoot
      ? "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, open_txn=?4, frag_id=?5, frag_off=?6, chain_hash=?7,"
        " state_root=?3 WHERE id=1"
      : "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, open_txn=?4, frag_id=?5, frag_off=?6, chain_hash=?7 WHERE id=1",
    -1, &pStmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)(pBatch->height + 1));
  sqlite3_bind_blob(pStmt, 2, blockHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
  sqlite3_bind_blob(pStmt, 7, chainHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
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
  memcpy(pCtx->chainHash, chainHash, HPPLITE_HASH_SIZE);
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
  rc = hppliteBkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return -2;
  {
    int saved = hppliteBkEnter(pCtx);
    rc = sqlite3_prepare_v2(pCtx->bkDb, "UPDATE _hpplite_meta SET rollup = ?1 WHERE id = 1", -1, &pStmt, NULL);
    if (rc == SQLITE_OK) {
      sqlite3_bind_text(pStmt, 1, zRollup, -1, SQLITE_STATIC);
      rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
      sqlite3_finalize(pStmt);
    }
    hppliteBkLeave(pCtx, saved);
  }
  rc = hppliteBkEnd(pCtx, rc);
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
  saved = hppliteBkEnter(pCtx);
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT data FROM _hpplite_posted WHERE height = ?1", -1, &pStmt, NULL) == SQLITE_OK) {
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height);
    if (sqlite3_step(pStmt) == SQLITE_ROW) {
      int n = sqlite3_column_bytes(pStmt, 0);
      out = malloc(n > 0 ? (size_t)n : 1);
      if (out) { memcpy(out, sqlite3_column_blob(pStmt, 0), (size_t)n); *pLen = (size_t)n; }
    }
    sqlite3_finalize(pStmt);
  }
  hppliteBkLeave(pCtx, saved);
  return out;
}

void hpplite_chain_hash_next(const unsigned char *pPrev, uint64_t height, const unsigned char *pBatchKeccak, unsigned char *pOut) {
  /* abi.encode(bytes32 prev, uint256 height, bytes32 batchHash): three 32-byte words */
  unsigned char buf[96];
  int i;
  memcpy(buf, pPrev, 32);
  memset(buf + 32, 0, 32);
  for (i = 0; i < 8; i++) buf[63 - i] = (unsigned char)(height >> (8 * i));
  memcpy(buf + 64, pBatchKeccak, 32);
  hpplite_keccak256(buf, sizeof(buf), pOut);
}

/* Page mode (#146): a batch of pages posted at height. The block hash chains
** the height, the previous hash and the root; the contract's rolling hash
** advances by the posted bytes as for any batch; the meta row moves; the
** posted copy is kept for a reorg. No log to prune, no trees. */
int hppliteAdvanceMetaPages(HppliteCtx *pCtx, uint64_t height, const unsigned char *posted, size_t nPosted, const unsigned char *root, int bSeriesOpen) {
  unsigned char blockHash[HPPLITE_HASH_SIZE], chainHash[HPPLITE_HASH_SIZE];
  sqlite3_stmt *pStmt = NULL;
  int rc, saved;
  if (!pCtx || !root) return SQLITE_MISUSE;
  if (height != pCtx->blockHeight) return SQLITE_MISMATCH;
  { SHA256_CTX h; unsigned char tag[12] = { 'H', 'P', 'P', 'B' }; int i;
    for (i = 0; i < 8; i++) tag[4 + i] = (unsigned char)(height >> (56 - 8 * i));
    hpplite_sha256_init(&h); hpplite_sha256_update(&h, tag, 12); hpplite_sha256_update(&h, pCtx->prevBlockHash, HPPLITE_HASH_SIZE);
    hpplite_sha256_update(&h, root, HPPLITE_HASH_SIZE); hpplite_sha256_final(&h, blockHash); }
  if (posted && nPosted) { unsigned char k[HPPLITE_HASH_SIZE]; hpplite_keccak256(posted, nPosted, k); hpplite_chain_hash_next(pCtx->chainHash, height, k, chainHash); }
  else memcpy(chainHash, pCtx->chainHash, HPPLITE_HASH_SIZE);
  rc = hppliteBkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  saved = hppliteBkEnter(pCtx);
  /* open_txn in page mode: a commit's pieces still to come (#152): the file is
  ** not at the state root until the last */
  rc = sqlite3_prepare_v2(pCtx->bkDb, "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, chain_hash=?3, state_root=?4, open_txn=?5, frag_id=0, frag_off=0, inflight_height=0, inflight_root=NULL WHERE id=1", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)(height + 1));
    sqlite3_bind_blob(pStmt, 2, blockHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    sqlite3_bind_blob(pStmt, 3, chainHash, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    sqlite3_bind_blob(pStmt, 4, root, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    sqlite3_bind_int(pStmt, 5, bSeriesOpen ? 1 : 0);
    rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_finalize(pStmt);
  }
  if (rc == SQLITE_OK && posted && nPosted) {
    rc = sqlite3_prepare_v2(pCtx->bkDb, "INSERT OR REPLACE INTO _hpplite_posted(height, data) VALUES(?1, ?2)", -1, &pStmt, NULL);
    if (rc == SQLITE_OK) { sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height); sqlite3_bind_blob(pStmt, 2, posted, (int)nPosted, SQLITE_STATIC); rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR; sqlite3_finalize(pStmt); }
    if (rc == SQLITE_OK) { rc = sqlite3_prepare_v2(pCtx->bkDb, "DELETE FROM _hpplite_posted WHERE height <= ?1", -1, &pStmt, NULL);
      if (rc == SQLITE_OK) { sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height - HPPLITE_POSTED_KEEP); rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR; sqlite3_finalize(pStmt); } }
  }
  hppliteBkLeave(pCtx, saved);
  TEST_FAIL_COMMIT(rc);
  rc = hppliteBkEnd(pCtx, rc);
  if (rc != SQLITE_OK) { hppliteLoadMeta(pCtx); return rc; }
  pCtx->blockHeight = height + 1;
  pCtx->openTxnId = bSeriesOpen ? 1 : 0; pCtx->fragId = 0; pCtx->fragOff = 0;
  memcpy(pCtx->prevBlockHash, blockHash, HPPLITE_HASH_SIZE);
  memcpy(pCtx->chainHash, chainHash, HPPLITE_HASH_SIZE);
  memcpy(pCtx->stateRoot, root, HPPLITE_HASH_SIZE);
  pCtx->inflightHeight = 0;
  { int i = pCtx->nRootAt % HPPLITE_ROOT_RING;        /* the root at this height, for hpplite_root_at (the verifier's checkpoint check) */
    pCtx->aRootAt[i].height = height; memcpy(pCtx->aRootAt[i].root, root, 32); pCtx->nRootAt++; }
  return SQLITE_OK;
}

int hppliteMarkInflightPages(HppliteCtx *pCtx, uint64_t height, const unsigned char *root) {
  sqlite3_stmt *pStmt = NULL;
  int rc, saved;
  rc = hppliteBkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  saved = hppliteBkEnter(pCtx);
  rc = sqlite3_prepare_v2(pCtx->bkDb, "UPDATE _hpplite_meta SET inflight_height=?1, inflight_root=?2 WHERE id=1", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) { sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height); sqlite3_bind_blob(pStmt, 2, root, 32, SQLITE_TRANSIENT); rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR; sqlite3_finalize(pStmt); }
  hppliteBkLeave(pCtx, saved);
  rc = hppliteBkEnd(pCtx, rc);
  if (rc == SQLITE_OK) { pCtx->inflightHeight = height; memcpy(pCtx->inflightRoot, root, 32); }
  return rc;
}

int hpplite_finalize_batch(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId) {
  int rc;
  if (!pCtx || !pBatch) return SQLITE_MISUSE;
  if (pBatch->height != pCtx->blockHeight) return SQLITE_MISMATCH;
  /* The state trees take the log in here, before the gated transaction
  ** (#141): hashing every record since the last flush is the long part of
  ** a finalize, it reads only committed log rows, which never change, and it
  ** writes nothing. Held under the gate it kept the application waiting for
  ** the whole of it. hppliteAdvanceMeta() syncs again inside the transaction
  ** and finds only what arrived meanwhile. */
  {
    int saved = hppliteBkEnter(pCtx);
    rc = hppliteTreeSync(pCtx);
    hppliteBkLeave(pCtx, saved);
    if (rc != SQLITE_OK) return rc;
  }
  rc = hppliteBkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  {
    int saved = hppliteBkEnter(pCtx);
    rc = hppliteAdvanceMeta(pCtx, pBatch, maxId, 1);
    hppliteBkLeave(pCtx, saved);
  }
  TEST_FAIL_COMMIT(rc);
  rc = hppliteBkEnd(pCtx, rc);
  /* a commit that failed (a full disk) rolled the file back: the height and
  ** the rest of the meta row that hppliteAdvanceMeta() set must follow, or the next
  ** flush posts the same rows again at the next height (#97) */
  if (rc != SQLITE_OK) hppliteLoadMeta(pCtx);
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
