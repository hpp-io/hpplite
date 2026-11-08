/*
** HPPLite - building and finalizing batches.
**
** Page capture (#146): a batch of pages posted at a height advances the
** meta row in the sidecar, keeps the posted copy for a reorg and, at a
** store, the row the anchor will name; the rolling chain hash; the
** verifier's ring of roots; a foreign height stepped over; the checkpoint.
*/

#include "hpplite_int.h"
#include "crypto.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int hppliteKeepUnanchored(HppliteCtx *pCtx, uint64_t height, const unsigned char *chainAfter, const unsigned char *root);


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

/* The row of _hpplite_unanchored for a batch a store holds (#164), inside the finalize's transaction */
int hppliteKeepUnanchored(HppliteCtx *pCtx, uint64_t height, const unsigned char *chainAfter, const unsigned char *root) {
  const HppliteAnchorRow *a = pCtx->pAnchorRow;
  sqlite3_stmt *pStmt = NULL;
  int rc;
  if (!a || !a->zRef) return SQLITE_MISUSE;
  rc = sqlite3_prepare_v2(pCtx->bkDb, "INSERT OR REPLACE INTO _hpplite_unanchored(height, hash, ref, receipt, chain_before, chain_after, root) VALUES(?1, ?2, ?3, ?4, ?7, ?5, ?6)", -1, &pStmt, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)height);
  sqlite3_bind_blob(pStmt, 2, a->hash, HPPLITE_HASH_SIZE, SQLITE_STATIC);
  sqlite3_bind_text(pStmt, 3, a->zRef, -1, SQLITE_STATIC);
  if (a->receipt && a->nReceipt) sqlite3_bind_blob(pStmt, 4, a->receipt, (int)a->nReceipt, SQLITE_STATIC); else sqlite3_bind_null(pStmt, 4);
  sqlite3_bind_blob(pStmt, 5, chainAfter, HPPLITE_HASH_SIZE, SQLITE_STATIC);
  if (root) sqlite3_bind_blob(pStmt, 6, root, HPPLITE_HASH_SIZE, SQLITE_STATIC); else sqlite3_bind_null(pStmt, 6);
  sqlite3_bind_blob(pStmt, 7, pCtx->chainHash, HPPLITE_HASH_SIZE, SQLITE_STATIC);   /* the mirror still holds the hash before this batch */
  rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
  sqlite3_finalize(pStmt);
  return rc;
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
  if (rc == SQLITE_OK && pCtx->pAnchorRow && posted && nPosted) rc = hppliteKeepUnanchored(pCtx, height, chainHash, root);   /* at a store, not yet anchored (#164) */
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

/* A replica started from a snapshot (#222): the bookkeeping as of its height - the block and chain
** hashes and the root it carries, checked by the caller against the chain - with no batch applied */
int hppliteSetMetaAt(HppliteCtx *pCtx, uint64_t height, const unsigned char *block, const unsigned char *chain, const unsigned char *root) {
  sqlite3_stmt *pStmt = NULL;
  int rc, saved;
  if (!pCtx || !block || !chain || !root || height == 0) return SQLITE_MISUSE;
  rc = hppliteBkBegin(pCtx, 1);
  if (rc != SQLITE_OK) return rc;
  saved = hppliteBkEnter(pCtx);
  rc = sqlite3_prepare_v2(pCtx->bkDb, "UPDATE _hpplite_meta SET next_height=?1, prev_block_hash=?2, chain_hash=?3, state_root=?4, open_txn=0, frag_id=0, frag_off=0, inflight_height=0, inflight_root=NULL WHERE id=1", -1, &pStmt, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_int64(pStmt, 1, (sqlite3_int64)(height + 1));
    sqlite3_bind_blob(pStmt, 2, block, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    sqlite3_bind_blob(pStmt, 3, chain, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    sqlite3_bind_blob(pStmt, 4, root, HPPLITE_HASH_SIZE, SQLITE_TRANSIENT);
    rc = sqlite3_step(pStmt) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
    sqlite3_finalize(pStmt);
  }
  hppliteBkLeave(pCtx, saved);
  rc = hppliteBkEnd(pCtx, rc);
  if (rc != SQLITE_OK) { hppliteLoadMeta(pCtx); return rc; }
  pCtx->blockHeight = height + 1;
  pCtx->openTxnId = 0; pCtx->fragId = 0; pCtx->fragOff = 0;
  memcpy(pCtx->prevBlockHash, block, HPPLITE_HASH_SIZE);
  memcpy(pCtx->chainHash, chain, HPPLITE_HASH_SIZE);
  memcpy(pCtx->stateRoot, root, HPPLITE_HASH_SIZE);
  pCtx->inflightHeight = 0;
  { int i = pCtx->nRootAt % HPPLITE_ROOT_RING;
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

/* The verifier's rebuilt root after a height, when it is among the last HPPLITE_ROOT_RING applied (#120) */
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
