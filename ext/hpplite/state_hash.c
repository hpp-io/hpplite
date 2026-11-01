/*
** HPPLite - the content hash of a database: the state root.
**
** A function of the file's user tables (schema and rows), not of the path
** taken to produce them: recomputed at any time, compared with what was
** posted. sqlite_* and _hpplite_* tables are excluded.
*/

#include "hpplite_int.h"
#include <string.h>
#include <stdio.h>

/*
** ============================================================================
** Content hash (state root)
** ============================================================================
*/

static void hashU64(SHA256_CTX *pH, uint64_t v) {
  uint8_t b[8];
  int i;
  for (i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - i * 8));
  hpplite_sha256_update(pH, b, 8);
}

static void hashBytes(SHA256_CTX *pH, const void *p, int n) {
  hashU64(pH, (uint64_t)n);
  if (n > 0) hpplite_sha256_update(pH, (const uint8_t*)p, (size_t)n);
}

static void hashText(SHA256_CTX *pH, const unsigned char *z) {
  hashBytes(pH, z, z ? (int)strlen((const char*)z) : 0);
}

static void hashValue(SHA256_CTX *pH, sqlite3_stmt *pStmt, int i) {
  switch (sqlite3_column_type(pStmt, i)) {
    case SQLITE_INTEGER: {
      hpplite_sha256_update(pH, (const uint8_t*)"I", 1);
      hashU64(pH, (uint64_t)sqlite3_column_int64(pStmt, i));
      break;
    }
    case SQLITE_FLOAT: {
      double d = sqlite3_column_double(pStmt, i);
      uint64_t bits;
      memcpy(&bits, &d, 8);
      hpplite_sha256_update(pH, (const uint8_t*)"F", 1);
      hashU64(pH, bits);
      break;
    }
    case SQLITE_TEXT:
      hpplite_sha256_update(pH, (const uint8_t*)"T", 1);
      hashBytes(pH, sqlite3_column_text(pStmt, i), sqlite3_column_bytes(pStmt, i));
      break;
    case SQLITE_BLOB:
      hpplite_sha256_update(pH, (const uint8_t*)"B", 1);
      hashBytes(pH, sqlite3_column_blob(pStmt, i), sqlite3_column_bytes(pStmt, i));
      break;
    default:
      hpplite_sha256_update(pH, (const uint8_t*)"N", 1);
      break;
  }
}

/*
** Hash every row of one table in a deterministic order.
*/
/* "WITHOUT ROWID" in any case, with any whitespace, anywhere after the column list */
static int isWithoutRowid(const char *zSql) {
  const char *z = zSql;
  if (!z) return 0;
  while ((z = strpbrk(z, "wW")) != NULL) {
    if (sqlite3_strnicmp(z, "without", 7) == 0) {
      const char *p = z + 7;
      if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (sqlite3_strnicmp(p, "rowid", 5) == 0) return 1;
      }
    }
    z++;
  }
  return 0;
}

static int hashTableRows(sqlite3 *db, SHA256_CTX *pH, const char *zName, const char *zSql) {
  sqlite3_stmt *pStmt;
  char *zQuery;
  int rc, nCol, i;
  int withoutRowid = isWithoutRowid(zSql);

  if (withoutRowid) {
    /* No rowid: order by every column (the primary key is among them) */
    sqlite3_str *pStr;
    zQuery = sqlite3_mprintf("SELECT * FROM \"%w\"", zName);
    rc = sqlite3_prepare_v2(db, zQuery, -1, &pStmt, NULL);
    sqlite3_free(zQuery);
    if (rc != SQLITE_OK) return rc;
    nCol = sqlite3_column_count(pStmt);
    sqlite3_finalize(pStmt);
    pStr = sqlite3_str_new(db);
    sqlite3_str_appendf(pStr, "SELECT * FROM \"%w\" ORDER BY 1", zName);
    for (i = 2; i <= nCol; i++) sqlite3_str_appendf(pStr, ",%d", i);
    zQuery = sqlite3_str_finish(pStr);
  } else {
    zQuery = sqlite3_mprintf("SELECT * FROM \"%w\" ORDER BY rowid", zName);
  }
  rc = sqlite3_prepare_v2(db, zQuery, -1, &pStmt, NULL);
  sqlite3_free(zQuery);
  if (rc != SQLITE_OK) return rc;

  nCol = sqlite3_column_count(pStmt);
  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {
    hpplite_sha256_update(pH, (const uint8_t*)"R", 1);
    for (i = 0; i < nCol; i++) hashValue(pH, pStmt, i);
  }
  sqlite3_finalize(pStmt);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int hpplite_db_hash(sqlite3 *db, unsigned char *pOut) {
  SHA256_CTX h;
  sqlite3_stmt *pStmt;
  int rc, nEntries = 0;

  hpplite_sha256_init(&h);
  rc = sqlite3_prepare_v2(db,
    "SELECT type, name, tbl_name, sql FROM sqlite_master"
    " WHERE name NOT LIKE 'sqlite\\_%' ESCAPE '\\'"
    "   AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\'"
    " ORDER BY type, name", -1, &pStmt, NULL);
  if (rc != SQLITE_OK) return rc;

  while ((rc = sqlite3_step(pStmt)) == SQLITE_ROW) {
    const unsigned char *zType = sqlite3_column_text(pStmt, 0);
    const unsigned char *zName = sqlite3_column_text(pStmt, 1);
    const unsigned char *zSql = sqlite3_column_text(pStmt, 3);
    nEntries++;
    hpplite_sha256_update(&h, (const uint8_t*)"S", 1);
    hashText(&h, zType);
    hashText(&h, zName);
    hashText(&h, sqlite3_column_text(pStmt, 2));
    hashText(&h, zSql);
    if (zType && strcmp((const char*)zType, "table") == 0
        && zSql && sqlite3_strnicmp((const char*)zSql, "CREATE VIRTUAL", 14) != 0) {
      rc = hashTableRows(db, &h, (const char*)zName, (const char*)zSql);
      if (rc != SQLITE_OK) break;
    }
  }
  sqlite3_finalize(pStmt);
  if (rc != SQLITE_DONE && rc != SQLITE_OK) return rc;

  /* the header fields an application sets, only when set: roots of
  ** databases that never touched them are unchanged */
  {
    static const char *aHdr[] = { "SELECT user_version FROM pragma_user_version", "U",
                                  "SELECT application_id FROM pragma_application_id", "A", NULL };
    int k;
    for (k = 0; aHdr[k]; k += 2) {
      sqlite3_int64 v = 0;
      if (sqlite3_prepare_v2(db, aHdr[k], -1, &pStmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(pStmt) == SQLITE_ROW) v = sqlite3_column_int64(pStmt, 0);
        sqlite3_finalize(pStmt);
      }
      if (v) {
        hpplite_sha256_update(&h, (const uint8_t*)aHdr[k + 1], 1);
        hashU64(&h, (uint64_t)v);
        nEntries++;
      }
    }
  }

  if (nEntries == 0) {
    memset(pOut, 0, HPPLITE_HASH_SIZE);  /* Genesis: no user schema */
  } else {
    hpplite_sha256_final(&h, pOut);
  }
  return SQLITE_OK;
}

/* Block hash: sha256(height || prevHash || [stateRoot] || tags || sql...) - the
** chain of hashes a replica checks batch by batch */
void hpplite_block_hash(const HppliteBatch *pBatch, const unsigned char *pPrevHash, unsigned char *pOut) {
  SHA256_CTX h;
  int i;
  hpplite_sha256_init(&h);
  hashU64(&h, pBatch->height);
  hpplite_sha256_update(&h, pPrevHash, HPPLITE_HASH_SIZE);
  if (pBatch->hasRoot) {
    hpplite_sha256_update(&h, pBatch->postStateRoot, HPPLITE_HASH_SIZE);
  }
  /* tagged, and only when set: hashes of batches without them are unchanged */
  if (pBatch->openTxn) hpplite_sha256_update(&h, (const uint8_t*)"O", 1);
  if (pBatch->contTxn) hpplite_sha256_update(&h, (const uint8_t*)"C", 1);
  if (pBatch->zDbName) { hpplite_sha256_update(&h, (const uint8_t*)"D", 1); hashText(&h, (const unsigned char*)pBatch->zDbName); }
  for (i = 0; i < pBatch->nTxns; i++) {
    hashText(&h, (const unsigned char*)pBatch->aTxns[i].zSql);
    if (pBatch->aTxns[i].mode) {
      /* tagged, and only when set: hashes of mode-0 batches are unchanged */
      hpplite_sha256_update(&h, (const uint8_t*)"M", 1);
      hashU64(&h, (uint64_t)pBatch->aTxns[i].mode);
    }
    if (pBatch->aTxns[i].now) {
      hpplite_sha256_update(&h, (const uint8_t*)"N", 1);
      hashU64(&h, (uint64_t)pBatch->aTxns[i].now);
    }
    if (pBatch->aTxns[i].frag) hpplite_sha256_update(&h, (const uint8_t*)"F", 1);
  }
  hpplite_sha256_final(&h, pOut);
}
