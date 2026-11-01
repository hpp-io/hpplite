/*
** HPPLite - maintained state trees (#9). See state_tree.h.
*/
#include "hpplite_int.h"
#include "state_tree.h"
#include "capture_rows.h"
#include "batch.h"
#include "smt.h"
#include <string.h>
#include <stdlib.h>

typedef struct TabCache {
  struct TabCache *pNext;
  HppliteRowsTab t;
  int ok;                                /* loaded; 0: the table cannot be read by key (stale instead) */
} TabCache;

typedef struct HppliteTree HppliteTree;
struct HppliteTree {
  HppliteCtx *pCtx;
  HppliteSmtStore *pMem;                 /* the overlay: nodes changed since the flush; kind EMPTY = removed */
  HppliteSmtStore layered;               /* reads: overlay, then the table; writes: overlay */
  HppliteSmtStore direct;                /* the table itself (rebuilds, in a write transaction) */
  sqlite3_int64 seq;                     /* the pending id the trees (with the overlay) reflect */
  sqlite3_int64 baseSeq;                 /* _hpplite_meta.tree_seq the overlay was built on */
  int stale;                             /* every tree is to be rebuilt from a scan */
  int gc;                                /* a table went or was renamed: nodes of tables gone are removed at the flush */
  int loaded;
  sqlite3_stmt *pGet, *pPut, *pDel;
  TabCache *pTabs;
};

static HppliteTree *treeOf(HppliteCtx *pCtx);

/* ------------------------------------------------------------- the table */

static int sqlGet(void *pArg, const char *zTab, const uint8_t *aPath, HppliteSmtNode *pNode) {
  HppliteTree *T = (HppliteTree *)pArg;
  int rc;
  pNode->kind = HPPLITE_SMT_EMPTY;
  if (!T->pGet) {
    rc = sqlite3_prepare_v3(T->pCtx->bkDb, "SELECT kind, key, hash FROM _hpplite_tree WHERE tab = ?1 AND path = ?2",
                            -1, SQLITE_PREPARE_PERSISTENT, &T->pGet, NULL);
    if (rc != SQLITE_OK) { T->pGet = NULL; return -1; }
  }
  sqlite3_bind_text(T->pGet, 1, zTab, -1, SQLITE_STATIC);
  sqlite3_bind_blob(T->pGet, 2, aPath, HPPLITE_SMT_PATH_LEN, SQLITE_STATIC);
  rc = sqlite3_step(T->pGet);
  if (rc == SQLITE_ROW) {
    pNode->kind = sqlite3_column_int(T->pGet, 0);
    if (sqlite3_column_bytes(T->pGet, 1) == HPPLITE_SMT_HASH) memcpy(pNode->key, sqlite3_column_blob(T->pGet, 1), HPPLITE_SMT_HASH);
    else memset(pNode->key, 0, HPPLITE_SMT_HASH);
    if (sqlite3_column_bytes(T->pGet, 2) == HPPLITE_SMT_HASH) memcpy(pNode->hash, sqlite3_column_blob(T->pGet, 2), HPPLITE_SMT_HASH);
    else memset(pNode->hash, 0, HPPLITE_SMT_HASH);
    rc = SQLITE_DONE;
  }
  sqlite3_reset(T->pGet);
  sqlite3_clear_bindings(T->pGet);
  return rc == SQLITE_DONE ? 0 : -1;
}

static int sqlPut(void *pArg, const char *zTab, const uint8_t *aPath, const HppliteSmtNode *pNode) {
  HppliteTree *T = (HppliteTree *)pArg;
  sqlite3_stmt *st;
  int rc;
  if (!pNode || pNode->kind == HPPLITE_SMT_EMPTY) {
    if (!T->pDel) {
      rc = sqlite3_prepare_v3(T->pCtx->bkDb, "DELETE FROM _hpplite_tree WHERE tab = ?1 AND path = ?2",
                              -1, SQLITE_PREPARE_PERSISTENT, &T->pDel, NULL);
      if (rc != SQLITE_OK) { T->pDel = NULL; return -1; }
    }
    st = T->pDel;
    sqlite3_bind_text(st, 1, zTab, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, aPath, HPPLITE_SMT_PATH_LEN, SQLITE_STATIC);
  } else {
    if (!T->pPut) {
      rc = sqlite3_prepare_v3(T->pCtx->bkDb, "INSERT OR REPLACE INTO _hpplite_tree(tab, path, kind, key, hash) VALUES(?1, ?2, ?3, ?4, ?5)",
                              -1, SQLITE_PREPARE_PERSISTENT, &T->pPut, NULL);
      if (rc != SQLITE_OK) { T->pPut = NULL; return -1; }
    }
    st = T->pPut;
    sqlite3_bind_text(st, 1, zTab, -1, SQLITE_STATIC);
    sqlite3_bind_blob(st, 2, aPath, HPPLITE_SMT_PATH_LEN, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, pNode->kind);
    if (pNode->kind == HPPLITE_SMT_LEAF) sqlite3_bind_blob(st, 4, pNode->key, HPPLITE_SMT_HASH, SQLITE_STATIC);
    else sqlite3_bind_null(st, 4);
    sqlite3_bind_blob(st, 5, pNode->hash, HPPLITE_SMT_HASH, SQLITE_STATIC);
  }
  rc = sqlite3_step(st);
  sqlite3_reset(st);
  sqlite3_clear_bindings(st);
  return rc == SQLITE_DONE ? 0 : -1;
}

/* ----------------------------------------------------------- the overlay */

static int layeredGet(void *pArg, const char *zTab, const uint8_t *aPath, HppliteSmtNode *pNode) {
  HppliteTree *T = (HppliteTree *)pArg;
  if (hpplite_smt_mem_store_lookup(T->pMem, zTab, aPath, pNode)) return 0;
  return sqlGet(pArg, zTab, aPath, pNode);
}

static int layeredPut(void *pArg, const char *zTab, const uint8_t *aPath, const HppliteSmtNode *pNode) {
  HppliteTree *T = (HppliteTree *)pArg;
  HppliteSmtNode gone;
  if (pNode) return T->pMem->xPut(T->pMem->pArg, zTab, aPath, pNode);
  memset(&gone, 0, sizeof gone);
  gone.kind = HPPLITE_SMT_EMPTY;         /* a recorded absence, over whatever the table holds */
  return T->pMem->xPut(T->pMem->pArg, zTab, aPath, &gone);
}

static void overlayDrop(HppliteTree *T) {
  hpplite_smt_mem_store_free(T->pMem);
  T->pMem = hpplite_smt_mem_store();
}

static void tabsFree(HppliteTree *T) {
  TabCache *c = T->pTabs;
  while (c) { TabCache *n = c->pNext; hpplite_rows_tab_free(&c->t); sqlite3_free(c); c = n; }
  T->pTabs = NULL;
}

static HppliteTree *treeOf(HppliteCtx *pCtx) {
  HppliteTree *T = pCtx->pTree;
  if (T) return T;
  T = sqlite3_malloc(sizeof *T);
  if (!T) return NULL;
  memset(T, 0, sizeof *T);
  T->pCtx = pCtx;
  T->pMem = hpplite_smt_mem_store();
  if (!T->pMem) { sqlite3_free(T); return NULL; }
  T->layered.pArg = T; T->layered.xGet = layeredGet; T->layered.xPut = layeredPut;
  T->direct.pArg = T; T->direct.xGet = sqlGet; T->direct.xPut = sqlPut;
  pCtx->pTree = T;
  return T;
}

void hppliteTreeFree(HppliteCtx *pCtx) {
  HppliteTree *T = pCtx ? pCtx->pTree : NULL;
  if (!T) return;
  sqlite3_finalize(T->pGet); sqlite3_finalize(T->pPut); sqlite3_finalize(T->pDel);
  hpplite_smt_mem_store_free(T->pMem);
  tabsFree(T);
  sqlite3_free(T);
  pCtx->pTree = NULL;
}

/* ------------------------------------------------------------------ state */

int hppliteTreeSchema(HppliteCtx *pCtx, sqlite3 *db) {
  int rc = hppliteInternalExec(pCtx, db,
    "CREATE TABLE IF NOT EXISTS _hpplite_tree("
    "  tab TEXT NOT NULL, path BLOB NOT NULL, kind INTEGER NOT NULL, key BLOB, hash BLOB NOT NULL,"
    "  PRIMARY KEY(tab, path)) WITHOUT ROWID");
  if (rc != SQLITE_OK) return rc;
  {
    /* tree_seq: the pending id the trees reflect; tree_stale: rebuilt from a scan at the next root (1 on a file from before) */
    static const char *aCol[] = { "tree_seq", "ALTER TABLE _hpplite_meta ADD COLUMN tree_seq INTEGER NOT NULL DEFAULT 0",
                                  "tree_stale", "ALTER TABLE _hpplite_meta ADD COLUMN tree_stale INTEGER NOT NULL DEFAULT 1", NULL };
    int k;
    for (k = 0; aCol[k] && rc == SQLITE_OK; k += 2) {
      sqlite3_stmt *q = NULL;
      char *z = sqlite3_mprintf("SELECT 1 FROM pragma_table_info('_hpplite_meta') WHERE name = %Q", aCol[k]);
      int has = 0;
      if (z && sqlite3_prepare_v2(db, z, -1, &q, NULL) == SQLITE_OK) has = (sqlite3_step(q) == SQLITE_ROW);
      sqlite3_finalize(q);
      sqlite3_free(z);
      if (!has) rc = hppliteInternalExec(pCtx, db, aCol[k + 1]);
    }
  }
  return rc;
}

/* meta's seq and stale; an overlay built on another base is dropped */
static int loadState(HppliteTree *T) {
  sqlite3_stmt *q = NULL;
  sqlite3_int64 seq = 0;
  int stale = 1, rc;
  rc = sqlite3_prepare_v2(T->pCtx->bkDb, "SELECT tree_seq, tree_stale FROM _hpplite_meta WHERE id = 1", -1, &q, NULL);
  if (rc != SQLITE_OK) return rc;
  if (sqlite3_step(q) == SQLITE_ROW) { seq = sqlite3_column_int64(q, 0); stale = sqlite3_column_int(q, 1); }
  sqlite3_finalize(q);
  if (!T->loaded || seq != T->baseSeq) {
    overlayDrop(T);
    T->seq = T->baseSeq = seq;
    T->stale = stale;
    T->gc = 0;
    T->loaded = 1;
  }
  return SQLITE_OK;
}

/* Does a statement leave every row of every table as it was? A table's
** rows change by INSERT/UPDATE/DELETE, by ALTER TABLE (its stored columns)
** and by VACUUM (rowids renumbered); creating or dropping objects does not,
** except CREATE TABLE ... AS. Unsure means no. */
static int keepsRows(const char *zSql, int *pGc) {
  static const char *aKeep[] = { "CREATE INDEX", "CREATE UNIQUE INDEX", "CREATE VIEW", "CREATE TRIGGER",
                                 "CREATE TEMP", "CREATE TEMPORARY", "DROP INDEX", "DROP VIEW", "DROP TRIGGER",
                                 "ANALYZE", "REINDEX", NULL };
  int k;
  while (*zSql == ' ' || *zSql == '\n' || *zSql == '\t' || *zSql == '\r') zSql++;
  if (sqlite3_strnicmp(zSql, "DROP TABLE", 10) == 0) { *pGc = 1; return 1; }
  if (sqlite3_strnicmp(zSql, "CREATE TABLE", 12) == 0) {
    const char *p;
    for (p = zSql; *p; p++) {
      if ((p[0] == ' ' || p[0] == ')' || p[0] == '\n') && sqlite3_strnicmp(p + 1, "AS", 2) == 0
          && (p[3] == ' ' || p[3] == '\n' || p[3] == '(' || p[3] == '\t')) return 0;   /* CREATE TABLE ... AS SELECT: rows */
    }
    return 1;
  }
  for (k = 0; aKeep[k]; k++) if (sqlite3_strnicmp(zSql, aKeep[k], (int)strlen(aKeep[k])) == 0) return 1;
  return 0;
}

static TabCache *tabFor(HppliteTree *T, const char *zTab) {
  TabCache *c;
  for (c = T->pTabs; c; c = c->pNext) if (sqlite3_stricmp(c->t.zName ? c->t.zName : "", zTab) == 0) return c;
  c = sqlite3_malloc(sizeof *c);
  if (!c) return NULL;
  memset(c, 0, sizeof *c);
  c->ok = hpplite_rows_tab_load(T->pCtx->bkDb, zTab, &c->t) == SQLITE_OK && c->t.nCol > 0 && (c->t.nKey || c->t.zRowid);
  if (!c->t.zName) c->t.zName = sqlite3_mprintf("%s", zTab);
  c->pNext = T->pTabs;
  T->pTabs = c;
  return c;
}

/* the leaf of one key, as the row is now */
static int touchKey(HppliteTree *T, TabCache *c, const unsigned char *aKey, int nKey) {
  unsigned char kh[32], vh[32];
  int present = hpplite_rows_leaf(T->pCtx->bkDb, &c->t, aKey, nKey, vh);
  if (present < 0) return SQLITE_ERROR;
  hpplite_rows_key_hash(aKey, nKey, kh);
  return hpplite_smt_set(&T->layered, c->t.zName, kh, present ? vh : NULL) == 0 ? SQLITE_OK : SQLITE_ERROR;
}

static int touchRecord(HppliteTree *T, const char *zTab, int op, const unsigned char *aRec, int nRec) {
  TabCache *c;
  unsigned char *a1 = NULL, *a2 = NULL;
  int n1 = 0, n2 = 0, rc;
  if (op == HPPLITE_ROW_SEQ) return SQLITE_OK;        /* sqlite_sequence is not in the root */
  if (T->stale) return SQLITE_OK;                      /* the rebuild will see it */
  c = tabFor(T, zTab);
  if (!c) return SQLITE_NOMEM;
  if (!c->ok) { T->stale = 1; return SQLITE_OK; }     /* not a table read by key (gone, or #72): the scan decides */
  rc = hpplite_rows_rec_keys(&c->t, op, aRec, nRec, &a1, &n1, &a2, &n2);
  if (rc != SQLITE_OK) { T->stale = 1; return SQLITE_OK; }   /* a record this table's shape does not fit: the scan is right */
  rc = touchKey(T, c, a1, n1);
  if (rc == SQLITE_OK && a2) rc = touchKey(T, c, a2, n2);
  sqlite3_free(a1); sqlite3_free(a2);
  return rc;
}

static void touchStatement(HppliteTree *T, const char *zSql) {
  int gc = 0;
  if (!keepsRows(zSql, &gc)) T->stale = 1;
  if (gc) T->gc = 1;
}

int hppliteTreeSync(HppliteCtx *pCtx) {
  HppliteTree *T = treeOf(pCtx);
  sqlite3_stmt *q = NULL;
  int rc, withRec = 1;
  if (!T) return SQLITE_NOMEM;
  if ((rc = loadState(T)) != SQLITE_OK) return rc;
  rc = sqlite3_prepare_v2(pCtx->bkDb, "SELECT id, sql, op, tbl, rec FROM _hpplite_pending WHERE id > ?1 ORDER BY id", -1, &q, NULL);
  if (rc != SQLITE_OK) {
    withRec = 0;                          /* a log without row capture's columns */
    rc = sqlite3_prepare_v2(pCtx->bkDb, "SELECT id, sql FROM _hpplite_pending WHERE id > ?1 ORDER BY id", -1, &q, NULL);
    if (rc != SQLITE_OK) return rc;
  }
  sqlite3_bind_int64(q, 1, T->seq);
  while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
    sqlite3_int64 id = sqlite3_column_int64(q, 0);
    const char *zSql = (const char *)sqlite3_column_text(q, 1);
    int op = withRec ? sqlite3_column_int(q, 2) : 0;
    if (withRec && op && sqlite3_column_type(q, 4) == SQLITE_BLOB) {
      rc = touchRecord(T, (const char *)sqlite3_column_text(q, 3), op,
                       sqlite3_column_blob(q, 4), sqlite3_column_bytes(q, 4));
      if (rc != SQLITE_OK) break;
    } else if (zSql && zSql[0]) {
      touchStatement(T, zSql);
    }
    T->seq = id;
  }
  sqlite3_finalize(q);
  tabsFree(T);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int hppliteTreeTouchEntries(HppliteCtx *pCtx, int n,
                            void (*xEntry)(void *pArg, int i, const char **pzSql, int *pMode), void *pArg) {
  HppliteTree *T = treeOf(pCtx);
  int i, rc = SQLITE_OK;
  if (!T) return SQLITE_NOMEM;
  if ((rc = loadState(T)) != SQLITE_OK) return rc;
  for (i = 0; i < n && rc == SQLITE_OK; i++) {
    const char *zSql = NULL;
    int mode = 0;
    xEntry(pArg, i, &zSql, &mode);
    if (!zSql) continue;
    if (mode & HPPLITE_MODE_ROW) {
      unsigned char *aBuf = NULL;
      char *zTab = NULL;
      int iRec = 0, nRec = 0, op = 0;
      if (hpplite_rows_entry_rec(zSql, &aBuf, &iRec, &nRec, &op, &zTab) == SQLITE_OK) {
        rc = touchRecord(T, zTab, op, aBuf + iRec, nRec);
      } else {
        T->stale = 1;
      }
      sqlite3_free(aBuf); sqlite3_free(zTab);
    } else {
      touchStatement(T, zSql);
    }
  }
  tabsFree(T);
  return rc;
}

/* ------------------------------------------------------------------- root */

static int tableRootScan(sqlite3 *db, const char *zTab, unsigned char *out) {
  HppliteRowsTab t;
  HppliteSmtLeaf *a = NULL;
  size_t n = 0;
  int rc = hpplite_rows_tab_load(db, zTab, &t);
  if (rc == SQLITE_OK) rc = hpplite_rows_table_leaves(db, &t, &a, &n);
  hpplite_rows_tab_free(&t);
  if (rc != SQLITE_OK) return rc;
  hpplite_test_rows_hashed_add((long)n);
  hpplite_smt_build_root(a, n, out);
  sqlite3_free(a);
  return SQLITE_OK;
}

static int treeTableRoot(void *pArg, sqlite3 *db, const char *zTab, unsigned char *out) {
  HppliteTree *T = (HppliteTree *)pArg;
  if (T->stale) return tableRootScan(db, zTab, out);
  return hpplite_smt_root(&T->layered, zTab, out) == 0 ? SQLITE_OK : SQLITE_ERROR;
}

int hppliteTreeRoot(HppliteCtx *pCtx, unsigned char *out) {
  HppliteTree *T;
  int rc = hppliteTreeSync(pCtx);
  if (rc != SQLITE_OK) return rc;
  T = pCtx->pTree;
  return hpplite_db_hash_v2(pCtx->bkDb, treeTableRoot, T, out);
}

/* ------------------------------------------------------------------ flush */

static int writeNode(void *pArg, const char *zTab, const uint8_t *aPath, const HppliteSmtNode *pNode) {
  return sqlPut(pArg, zTab, aPath, pNode);
}

static int rebuildAll(HppliteTree *T) {
  sqlite3 *db = T->pCtx->bkDb;
  sqlite3_stmt *q = NULL;
  int rc = hppliteInternalExec(T->pCtx, db, "DELETE FROM _hpplite_tree");
  if (rc != SQLITE_OK) return rc;
  rc = sqlite3_prepare_v2(db,
    "SELECT name, sql FROM sqlite_master WHERE type = 'table'"
    " AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\'", -1, &q, NULL);
  if (rc != SQLITE_OK) return rc;
  while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
    const char *zName = (const char *)sqlite3_column_text(q, 0);
    const char *zSql = (const char *)sqlite3_column_text(q, 1);
    HppliteRowsTab t;
    HppliteSmtLeaf *a = NULL;
    size_t n = 0;
    unsigned char root[32];
    if (!zSql || sqlite3_strnicmp(zSql, "CREATE VIRTUAL", 14) == 0) continue;
    rc = hpplite_rows_tab_load(db, zName, &t);
    if (rc == SQLITE_OK) rc = hpplite_rows_table_leaves(db, &t, &a, &n);
    hpplite_rows_tab_free(&t);
    if (rc == SQLITE_OK) {
      hpplite_test_rows_hashed_add((long)n);
      rc = hpplite_smt_build_store(&T->direct, zName, a, n, root) == 0 ? SQLITE_OK : SQLITE_ERROR;
    }
    sqlite3_free(a);
    if (rc != SQLITE_OK) break;
    rc = SQLITE_ROW;
  }
  sqlite3_finalize(q);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

int hppliteTreeFlush(HppliteCtx *pCtx) {
  HppliteTree *T;
  sqlite3_stmt *q = NULL;
  sqlite3_int64 maxId = 0, newSeq;
  int rc = hppliteTreeSync(pCtx);
  if (rc != SQLITE_OK) return rc;
  T = pCtx->pTree;
  if (T->stale) {
    rc = rebuildAll(T);
  } else {
    rc = hpplite_smt_mem_store_each(T->pMem, NULL, writeNode, T) == 0 ? SQLITE_OK : SQLITE_ERROR;
    if (rc == SQLITE_OK && T->gc) {
      rc = hppliteInternalExec(pCtx, pCtx->bkDb,
        "DELETE FROM _hpplite_tree WHERE tab NOT IN (SELECT name FROM sqlite_master WHERE type = 'table')");
    }
  }
  if (rc != SQLITE_OK) return rc;
  /* pending ids restart when the log empties: the seq can never be above the log's last id */
  if (sqlite3_prepare_v2(pCtx->bkDb, "SELECT COALESCE(MAX(id), 0) FROM _hpplite_pending", -1, &q, NULL) == SQLITE_OK) {
    if (sqlite3_step(q) == SQLITE_ROW) maxId = sqlite3_column_int64(q, 0);
  }
  sqlite3_finalize(q);
  newSeq = T->seq < maxId ? T->seq : maxId;
  rc = sqlite3_prepare_v2(pCtx->bkDb, "UPDATE _hpplite_meta SET tree_seq = ?1, tree_stale = 0 WHERE id = 1", -1, &q, NULL);
  if (rc != SQLITE_OK) return rc;
  sqlite3_bind_int64(q, 1, newSeq);
  rc = sqlite3_step(q) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
  sqlite3_finalize(q);
  if (rc != SQLITE_OK) return rc;
  overlayDrop(T);
  T->seq = T->baseSeq = newSeq;
  T->stale = 0;
  T->gc = 0;
  return SQLITE_OK;
}
