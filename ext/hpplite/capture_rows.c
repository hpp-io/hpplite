/*
** HPPLite row capture (capture engine v2, #19; doc/design-row-capture.md)
**
** A record is what one changed row became, in a compact typed encoding:
**
**   value  := tag payload      tag 0 NULL, 1 integer (zigzag varint),
**                              2 real (8 bytes, IEEE 754, big-endian),
**                              3 text, 4 blob (varint length, bytes)
**   insert := varint n, n values      key (rowid, or the primary key of a
**                                      WITHOUT ROWID table) then every
**                                      stored column
**   update := varint nKey, old key values, byte keyChanged,
**             [new key values], varint nCol, bitmap (nCol bits),
**             the changed columns' new values
**   delete := varint n, key values
**
** Columns are the table's stored columns in order (generated ones are
** computed by the replica); the replica's table has the same columns at that
** point of the log, schema changes being in the same log.
*/
#include "hpplite_sqlite.h"
#include "hpplite_int.h"
#include "batch.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- encoding */

typedef struct Rec { unsigned char *a; int n, cap, oom; } Rec;

static void recGrow(Rec *r, int n) {
  if (r->oom || r->n + n <= r->cap) return;
  int cap = (r->cap + n) * 2;
  unsigned char *p = sqlite3_realloc(r->a, cap);
  if (!p) { r->oom = 1; return; }
  r->a = p; r->cap = cap;
}
static void recByte(Rec *r, unsigned char b) { recGrow(r, 1); if (!r->oom) r->a[r->n++] = b; }
static void recVarint(Rec *r, sqlite3_uint64 v) {
  recGrow(r, 10);
  if (r->oom) return;
  do { unsigned char b = v & 0x7f; v >>= 7; r->a[r->n++] = b | (v ? 0x80 : 0); } while (v);
}
static void recBytes(Rec *r, const void *p, int n) {
  recVarint(r, (sqlite3_uint64)n);
  recGrow(r, n);
  if (!r->oom && n > 0) { memcpy(r->a + r->n, p, (size_t)n); r->n += n; }
}
static void recValue(Rec *r, sqlite3_value *v) {
  switch (sqlite3_value_type(v)) {
    case SQLITE_INTEGER: {
      sqlite3_int64 x = sqlite3_value_int64(v);
      recByte(r, 1);
      recVarint(r, ((sqlite3_uint64)x << 1) ^ (sqlite3_uint64)(x >> 63));
      break;
    }
    case SQLITE_FLOAT: {
      double d = sqlite3_value_double(v);
      sqlite3_uint64 u;
      int i;
      memcpy(&u, &d, 8);
      recByte(r, 2);
      for (i = 7; i >= 0; i--) recByte(r, (unsigned char)(u >> (i * 8)));
      break;
    }
    case SQLITE_TEXT:
      recByte(r, 3);
      recBytes(r, sqlite3_value_text(v), sqlite3_value_bytes(v));
      break;
    case SQLITE_BLOB:
      recByte(r, 4);
      recBytes(r, sqlite3_value_blob(v), sqlite3_value_bytes(v));
      break;
    default:
      recByte(r, 0);
  }
}
static void recResult(sqlite3_context *c, Rec *r) {
  if (r->oom) { sqlite3_free(r->a); sqlite3_result_error_nomem(c); return; }
  sqlite3_result_blob(c, r->a, r->n, sqlite3_free);
}

/* hpplite_rec(v, ...): an insert or delete record */
static void fnRec(sqlite3_context *c, int n, sqlite3_value **v) {
  Rec r = {0};
  int i;
  recVarint(&r, (sqlite3_uint64)n);
  for (i = 0; i < n; i++) recValue(&r, v[i]);
  recResult(c, &r);
}

/* Same value, the way the row would store it (NULL IS NULL; 1 IS 1.0 is not:
** types are part of the value) */
static int sameValue(sqlite3_value *a, sqlite3_value *b) {
  int t = sqlite3_value_type(a);
  if (t != sqlite3_value_type(b)) return 0;
  switch (t) {
    case SQLITE_NULL: return 1;
    case SQLITE_INTEGER: return sqlite3_value_int64(a) == sqlite3_value_int64(b);
    case SQLITE_FLOAT: { double x = sqlite3_value_double(a), y = sqlite3_value_double(b); return memcmp(&x, &y, 8) == 0; }
    default: {
      int n = sqlite3_value_bytes(a);
      return n == sqlite3_value_bytes(b) && (n == 0 || memcmp(sqlite3_value_blob(a), sqlite3_value_blob(b), (size_t)n) == 0);
    }
  }
}

/* hpplite_upd(nKey, old key..., new key..., old c1, new c1, ...): an update record */
static void fnUpd(sqlite3_context *c, int n, sqlite3_value **v) {
  Rec r = {0};
  int nKey, i, nCol, keyChanged = 0;
  if (n < 1) { sqlite3_result_error(c, "hpplite_upd: arguments", -1); return; }   /* callable from any SQL (#70) */
  nKey = sqlite3_value_int(v[0]);
  if (nKey < 0 || 1 + 2 * nKey > n || (n - 1 - 2 * nKey) % 2) { sqlite3_result_error(c, "hpplite_upd: arguments", -1); return; }
  nCol = (n - 1 - 2 * nKey) / 2;
  recVarint(&r, (sqlite3_uint64)nKey);
  for (i = 0; i < nKey; i++) recValue(&r, v[1 + i]);
  for (i = 0; i < nKey; i++) if (!sameValue(v[1 + i], v[1 + nKey + i])) keyChanged = 1;
  recByte(&r, (unsigned char)keyChanged);
  if (keyChanged) for (i = 0; i < nKey; i++) recValue(&r, v[1 + nKey + i]);
  recVarint(&r, (sqlite3_uint64)nCol);
  {
    sqlite3_value **pc = v + 1 + 2 * nKey;
    int nb = (nCol + 7) / 8, at;
    recGrow(&r, nb);
    if (!r.oom) {
      at = r.n;
      memset(r.a + at, 0, (size_t)nb);
      r.n += nb;
      for (i = 0; i < nCol; i++) if (!sameValue(pc[2 * i], pc[2 * i + 1])) r.a[at + i / 8] |= (unsigned char)(1 << (i % 8));
      for (i = 0; i < nCol; i++) if (r.a[at + i / 8] & (1 << (i % 8))) recValue(&r, pc[2 * i + 1]);
    }
  }
  recResult(c, &r);
}

/* hpplite_cap(): capture is on (off while a statement is logged as text, and
** while HPPLite runs its own statements - replay among them) */
static void fnCap(sqlite3_context *c, int n, sqlite3_value **v) {
  HppliteCtx *p = sqlite3_user_data(c);
  (void)n; (void)v;
  sqlite3_result_int(c, p->bRows && !p->bInternal && !p->bRowsCapOff);
}
/* hpplite_grp() / hpplite_note(id): transaction groups as v1 names them -
** the first row of a transaction stores 0 (its own id), the rest that id */
static void fnGrp(sqlite3_context *c, int n, sqlite3_value **v) {
  HppliteCtx *p = sqlite3_user_data(c);
  (void)n; (void)v;
  sqlite3_result_int64(c, p->curTxn);
}
/* hpplite_mode(): a record, under the writer's foreign keys - replay runs
** it with the same setting, so a cascade already recorded finds nothing
** left to do there, and one the writer did not do is not done either */
static void fnMode(sqlite3_context *c, int n, sqlite3_value **v) {
  HppliteCtx *p = sqlite3_user_data(c);
  int fk = 0;
  (void)n; (void)v;
  sqlite3_db_config(p->db, SQLITE_DBCONFIG_ENABLE_FKEY, -1, &fk);
  /* a record carries the connection pragmas too (#28): whether its row is
  ** accepted (checks ignored, LIKE's case in a CHECK), and whether the
  ** schema the writer edited under writable_schema has been reloaded */
  sqlite3_result_int(c, HPPLITE_MODE_ROW | (fk ? HPPLITE_MODE_FOREIGN_KEYS : 0) | (p->pragmaMode & HPPLITE_MODE_PRAGMAS));
}
static void fnNote(sqlite3_context *c, int n, sqlite3_value **v) {
  HppliteCtx *p = sqlite3_user_data(c);
  (void)n;
  if (p->curTxn == 0) p->curTxn = sqlite3_value_int64(v[0]);
  sqlite3_result_null(c);
}

/* ---------------------------------------------------------------- triggers */

/* A table as capture sees it: its stored columns and its key */
typedef struct Tab {
  char *zName;
  int nCol; char **azCol;       /* stored columns, in order */
  int nKey; char **azKey;       /* key columns (WITHOUT ROWID); 0 = rowid */
  int ipk;                      /* rowid table whose INTEGER PRIMARY KEY is the rowid: its index, else -1 */
  const char *zRowid;           /* the rowid's name that no column takes: rowid, _rowid_ or oid; NULL when
                                ** all three are columns (the table cannot be addressed by row, #72) */
} Tab;

static void tabFree(Tab *t) {
  int i;
  for (i = 0; i < t->nCol; i++) sqlite3_free(t->azCol[i]);
  for (i = 0; i < t->nKey; i++) sqlite3_free(t->azKey[i]);
  sqlite3_free(t->azCol); sqlite3_free(t->azKey); sqlite3_free(t->zName);
  memset(t, 0, sizeof(*t));
}

static int tabLoad(sqlite3 *db, const char *zName, Tab *t) {
  sqlite3_stmt *q = NULL;
  char *z;
  int rc, nPk = 0, i, hasRowid = 0;
  memset(t, 0, sizeof(*t));
  t->ipk = -1;
  t->zName = sqlite3_mprintf("%s", zName);
  /* WITHOUT ROWID or not, as the schema says (a column named rowid would
  ** make "SELECT rowid" succeed on either, #72). main's table: a TEMP table
  ** of the same name would answer first for an unqualified name (#67) */
  z = sqlite3_mprintf("SELECT wr FROM pragma_table_list WHERE schema = 'main' AND name = %Q", zName);
  if (sqlite3_prepare_v2(db, z, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) hasRowid = sqlite3_column_int(q, 0) == 0;
  sqlite3_finalize(q); q = NULL;
  sqlite3_free(z);
  z = sqlite3_mprintf("SELECT name, type, pk, hidden FROM pragma_table_xinfo(%Q, 'main') ORDER BY cid", zName);
  rc = sqlite3_prepare_v2(db, z, -1, &q, NULL);
  sqlite3_free(z);
  if (rc != SQLITE_OK) return rc;
  {
    int cap = 0, pkCap = 0;
    char **azPkByOrder = NULL;
    int *aPkOrder = NULL;
    while (sqlite3_step(q) == SQLITE_ROW) {
      int hidden = sqlite3_column_int(q, 3), pk = sqlite3_column_int(q, 2);
      const char *zCol = (const char *)sqlite3_column_text(q, 0);
      const char *zType = (const char *)sqlite3_column_text(q, 1);
      if (hidden == 2 || hidden == 3) continue;           /* generated: the replica computes it */
      if (t->nCol == cap) { cap = cap ? cap * 2 : 8; t->azCol = sqlite3_realloc(t->azCol, cap * (int)sizeof(char *)); }
      t->azCol[t->nCol] = sqlite3_mprintf("%s", zCol);
      if (pk > 0) {
        if (nPk == pkCap) {
          pkCap = pkCap ? pkCap * 2 : 4;
          azPkByOrder = sqlite3_realloc(azPkByOrder, pkCap * (int)sizeof(char *));
          aPkOrder = sqlite3_realloc(aPkOrder, pkCap * (int)sizeof(int));
        }
        azPkByOrder[nPk] = t->azCol[t->nCol]; aPkOrder[nPk] = pk; nPk++;
        if (hasRowid && zType && sqlite3_stricmp(zType, "INTEGER") == 0) t->ipk = t->nCol;
      }
      t->nCol++;
    }
    sqlite3_finalize(q);
    if (hasRowid && nPk != 1) t->ipk = -1;
    if (t->ipk >= 0) {
      /* INTEGER PRIMARY KEY DESC is not the rowid (SQLite's documented
      ** quirk): it gets an index of its own, origin 'pk' (#73) */
      z = sqlite3_mprintf("SELECT 1 FROM pragma_index_list(%Q, 'main') WHERE origin = 'pk'", zName);
      if (sqlite3_prepare_v2(db, z, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) t->ipk = -1;
      sqlite3_finalize(q); q = NULL;
      sqlite3_free(z);
    }
    if (!hasRowid) {
      /* the key is the primary key, in its declared order */
      t->nKey = nPk;
      t->azKey = sqlite3_malloc((nPk ? nPk : 1) * (int)sizeof(char *));
      for (i = 0; i < nPk; i++) {
        int j, at = 0;
        for (j = 0; j < nPk; j++) if (aPkOrder[j] < aPkOrder[i]) at++;
        t->azKey[at] = sqlite3_mprintf("%s", azPkByOrder[i]);
      }
    }
    sqlite3_free(azPkByOrder); sqlite3_free(aPkOrder);
  }
  if (hasRowid) {
    /* the rowid under a name no column shadows */
    static const char *aAlias[] = { "rowid", "_rowid_", "oid" };
    int a;
    for (a = 0; a < 3 && !t->zRowid; a++) {
      for (i = 0; i < t->nCol && sqlite3_stricmp(t->azCol[i], aAlias[a]) != 0; i++) {}
      if (i == t->nCol) t->zRowid = aAlias[a];
    }
  }
  return SQLITE_OK;
}

/* "new.k1, new.k2" / "new.rowid" */
static char *keyList(const Tab *t, const char *zRow) {
  char *z = NULL;
  int i;
  if (t->nKey == 0) return sqlite3_mprintf("%s.%s", zRow, t->zRowid);
  for (i = 0; i < t->nKey; i++) {
    char *n = sqlite3_mprintf("%z%s%s.\"%w\"", z, i ? ", " : "", zRow, t->azKey[i]);
    z = n;
  }
  return z;
}

static int buildTriggers(HppliteCtx *pCtx, const Tab *t, int k) {
  char *zNewKey = keyList(t, "new"), *zOldKey = keyList(t, "old");
  char *zIns = NULL, *zUpd = NULL, *zSql, *zNewRowid = t->nKey ? NULL : sqlite3_mprintf("new.%s", t->zRowid);
  int i, rc, nKey = t->nKey ? t->nKey : 1;
  for (i = 0; i < t->nCol; i++) {
    zIns = sqlite3_mprintf("%z, new.\"%w\"", zIns, t->azCol[i]);
    zUpd = sqlite3_mprintf("%z, old.\"%w\", new.\"%w\"", zUpd, t->azCol[i], t->azCol[i]);
  }
  /* the insert record: key, then the stored columns (a WITHOUT ROWID
  ** table's key is among its columns: not repeated) */
  zSql = sqlite3_mprintf(
    "CREATE TEMP TRIGGER \"hpplite_cap_i%d\" AFTER INSERT ON main.\"%w\" WHEN hpplite_cap() BEGIN"
    " INSERT INTO _hpplite_pending(txn, sql, mode, now, tbl, op, rec)"
    "  VALUES(hpplite_grp(), '', hpplite_mode(), 0, %Q, %d, hpplite_rec(%s%s));"
    " SELECT hpplite_note(last_insert_rowid()); END;"
    "CREATE TEMP TRIGGER \"hpplite_cap_u%d\" AFTER UPDATE ON main.\"%w\" WHEN hpplite_cap() BEGIN"
    " INSERT INTO _hpplite_pending(txn, sql, mode, now, tbl, op, rec)"
    "  VALUES(hpplite_grp(), '', hpplite_mode(), 0, %Q, %d, hpplite_upd(%d, %s, %s%s));"
    " SELECT hpplite_note(last_insert_rowid()); END;"
    "CREATE TEMP TRIGGER \"hpplite_cap_d%d\" AFTER DELETE ON main.\"%w\" WHEN hpplite_cap() BEGIN"
    " INSERT INTO _hpplite_pending(txn, sql, mode, now, tbl, op, rec)"
    "  VALUES(hpplite_grp(), '', hpplite_mode(), 0, %Q, %d, hpplite_rec(%s));"
    " SELECT hpplite_note(last_insert_rowid()); END;",
    k, t->zName, t->zName, HPPLITE_ROW_INSERT, t->nKey ? "" : zNewRowid, t->nKey ? (zIns ? zIns + 2 : "") : (zIns ? zIns : ""),
    k, t->zName, t->zName, HPPLITE_ROW_UPDATE, nKey, zOldKey, zNewKey, zUpd ? zUpd : "",
    k, t->zName, t->zName, HPPLITE_ROW_DELETE, zOldKey);
  rc = zSql ? sqlite3_exec(pCtx->db, zSql, NULL, NULL, NULL) : SQLITE_NOMEM;
  sqlite3_free(zSql); sqlite3_free(zIns); sqlite3_free(zUpd); sqlite3_free(zNewKey); sqlite3_free(zOldKey); sqlite3_free(zNewRowid);
  return rc;
}

static int schemaVersionCached(HppliteCtx *pCtx, sqlite3_stmt **ppStmt, const char *zPragma) {
  int v = -1, saved = hppliteInternalBegin(pCtx);   /* ours: not traced as the application's */
  if (!*ppStmt && sqlite3_prepare_v3(pCtx->db, zPragma, -1, SQLITE_PREPARE_PERSISTENT, ppStmt, NULL) != SQLITE_OK) {
    *ppStmt = NULL;
    hppliteInternalEnd(pCtx, saved);
    return -1;
  }
  if (sqlite3_step(*ppStmt) == SQLITE_ROW) v = sqlite3_column_int(*ppStmt, 0);
  sqlite3_reset(*ppStmt);
  hppliteInternalEnd(pCtx, saved);
  return v;
}
static int schemaVersionOf(HppliteCtx *pCtx) {
  return schemaVersionCached(pCtx, &pCtx->pRowsSchemaStmt, "PRAGMA main.schema_version");
}
/* temp's: the triggers live there, and a ROLLBACK of the transaction they
** were rebuilt in brings the old ones back, temp's version with them (#88) */
static int tempSchemaVersionOf(HppliteCtx *pCtx) {
  return schemaVersionCached(pCtx, &pCtx->pRowsTempSchemaStmt, "PRAGMA temp.schema_version");
}

/* Does zSql mention zName as a whole identifier? (case-insensitive; a
** string or comment counts too: a false hit only costs a text record) */
static int mentions(const char *zSql, const char *zName) {
  int n = (int)strlen(zName);
  const char *z;
  if (n == 0) return 0;
  for (z = zSql; *z; z++) {
    if (sqlite3_strnicmp(z, zName, n) != 0) continue;
    if (z > zSql && (isalnum((unsigned char)z[-1]) || z[-1] == '_' || z[-1] == '$' || (unsigned char)z[-1] >= 0x80)) continue;
    if (isalnum((unsigned char)z[n]) || z[n] == '_' || z[n] == '$' || (unsigned char)z[n] >= 0x80) continue;
    return 1;
  }
  return 0;
}

static int textOnlyHas(const char *zList, const char *zName) {
  const char *z;
  for (z = zList; z && *z; z += strlen(z) + 1) if (sqlite3_stricmp(z, zName) == 0) return 1;
  return 0;
}

static int textOnlyMentioned(const char *zList, const char *zSql) {
  const char *z;
  for (z = zList; z && *z; z += strlen(z) + 1) if (mentions(zSql, z)) return 1;
  return 0;
}

static int textOnlyAdd(char **pzList, int *pnList, const char *zName) {
  int n = (int)strlen(zName) + 1;
  char *z = sqlite3_realloc(*pzList, *pnList + n + 1);
  if (!z) return SQLITE_NOMEM;
  memcpy(z + *pnList, zName, n);
  *pnList += n;
  z[*pnList] = 0;
  *pzList = z;
  return SQLITE_OK;
}

/* The names whose writes are text: virtual tables, the tables given (ones
** the triggers cannot be built for), then - to a fixpoint - every table or
** view with a trigger that mentions one already listed */
static int loadTextOnly(HppliteCtx *pCtx, const char *zSeed) {
  sqlite3_stmt *q = NULL;
  char *zList = NULL;
  const char *z;
  int nList = 0, rc, grew;
  /* sqlite_sequence carries no triggers either: a write to it is text */
  rc = textOnlyAdd(&zList, &nList, "sqlite_sequence");
  for (z = zSeed; rc == SQLITE_OK && z && *z; z += strlen(z) + 1) rc = textOnlyAdd(&zList, &nList, z);
  if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(pCtx->db,
    "SELECT name FROM pragma_table_list WHERE schema = 'main' AND type = 'virtual'", -1, &q, NULL);
  while (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
    rc = textOnlyAdd(&zList, &nList, (const char *)sqlite3_column_text(q, 0));
  }
  sqlite3_finalize(q);
  q = NULL;
  if (rc == SQLITE_OK && zList) {
    sqlite3_stmt *fk = NULL;
    rc = sqlite3_prepare_v2(pCtx->db,
      "SELECT tbl_name, sql FROM main.sqlite_schema WHERE type = 'trigger' AND sql IS NOT NULL"
      " UNION ALL SELECT tbl_name, sql FROM temp.sqlite_schema WHERE type = 'trigger' AND sql IS NOT NULL"
      " AND name NOT LIKE 'hpplite\\_cap\\_%' ESCAPE '\\'", -1, &q, NULL);     /* the application's TEMP triggers too (#76) */
    /* a foreign key action writes the child when the parent changes */
    if (rc == SQLITE_OK) rc = sqlite3_prepare_v2(pCtx->db,
      "SELECT f.\"table\", m.name FROM main.sqlite_schema m, pragma_foreign_key_list(m.name) f"
      " WHERE m.type = 'table' AND (f.on_delete NOT IN ('NO ACTION', 'RESTRICT') OR f.on_update NOT IN ('NO ACTION', 'RESTRICT'))",
      -1, &fk, NULL);
    do {
      grew = 0;
      while (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
        const char *zName = (const char *)sqlite3_column_text(q, 0);
        const char *zBody = (const char *)sqlite3_column_text(q, 1);
        if (!zName || !zBody || textOnlyHas(zList, zName) || !textOnlyMentioned(zList, zBody)) continue;
        rc = textOnlyAdd(&zList, &nList, zName);
        grew = 1;
      }
      while (rc == SQLITE_OK && sqlite3_step(fk) == SQLITE_ROW) {
        const char *zParent = (const char *)sqlite3_column_text(fk, 0);
        const char *zChild = (const char *)sqlite3_column_text(fk, 1);
        if (!zParent || !zChild || textOnlyHas(zList, zParent) || !textOnlyHas(zList, zChild)) continue;
        rc = textOnlyAdd(&zList, &nList, zParent);
        grew = 1;
      }
      if (q) sqlite3_reset(q);
      if (fk) sqlite3_reset(fk);
    } while (rc == SQLITE_OK && grew);
    sqlite3_finalize(q);
    sqlite3_finalize(fk);
  }
  if (rc != SQLITE_OK) { sqlite3_free(zList); return rc; }
  sqlite3_free(pCtx->zRowsTextOnly);
  pCtx->zRowsTextOnly = zList;
  return SQLITE_OK;
}

/* Drop every capture trigger and build them again for the current schema.
** A table they cannot be built for is logged as text instead, the v1 way:
** one too wide for this SQLite's function argument limit (127 before 3.48,
** on distribution SQLites; a record passes every column as an argument, an
** update record two per column, #65). */
static int dropAllTriggers(HppliteCtx *pCtx) {
  sqlite3_stmt *q = NULL;
  char *zDrop = NULL;
  int rc = sqlite3_prepare_v2(pCtx->db,
    "SELECT name FROM temp.sqlite_schema WHERE type = 'trigger' AND name LIKE 'hpplite_cap_%'", -1, &q, NULL);
  while (rc == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
    zDrop = sqlite3_mprintf("%zDROP TRIGGER temp.\"%w\";", zDrop, (const char *)sqlite3_column_text(q, 0));
  }
  sqlite3_finalize(q);
  if (rc == SQLITE_OK && zDrop) rc = sqlite3_exec(pCtx->db, zDrop, NULL, NULL, NULL);
  sqlite3_free(zDrop);
  return rc;
}

static int rebuild(HppliteCtx *pCtx) {
  sqlite3_stmt *q = NULL;
  char **azName = NULL, *zSkip = NULL;
  int n = 0, cap = 0, nSkip = 0, i, rc, v, saved = hppliteInternalBegin(pCtx);
  int argLimit = sqlite3_limit(pCtx->db, SQLITE_LIMIT_FUNCTION_ARG, -1);
  v = schemaVersionOf(pCtx);
  rc = dropAllTriggers(pCtx);
  /* every ordinary table of main but SQLite's and ours. Not virtual tables,
  ** which cannot carry triggers, nor their shadow tables: a trigger there
  ** makes the module's own flush open a savepoint that flushes again,
  ** without end (FTS5). Writes to them are logged as text instead. */
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(pCtx->db,
      "SELECT name FROM pragma_table_list WHERE schema = 'main' AND type = 'table'"
      " AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\' AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\' ORDER BY name",
      -1, &q, NULL);
    while (rc == SQLITE_OK) {
      /* a listing that fails (the schema stale and pinned by a running
      ** statement, #108) is a failed rebuild, not a database without tables:
      ** triggers for none would cover nothing and say "covered" */
      int s = sqlite3_step(q);
      if (s != SQLITE_ROW) { if (s != SQLITE_DONE) rc = s; break; }
      if (n == cap) { cap = cap ? cap * 2 : 16; azName = sqlite3_realloc(azName, cap * (int)sizeof(char *)); }
      azName[n++] = sqlite3_mprintf("%s", (const char *)sqlite3_column_text(q, 0));
    }
    sqlite3_finalize(q);
  }
  for (i = 0; i < n; i++) {
    if (rc == SQLITE_OK) {
      Tab t;
      rc = tabLoad(pCtx->db, azName[i], &t);
      if (rc == SQLITE_OK) {
        int nArgs = 1 + 2 * (t.nKey ? t.nKey : 1) + 2 * t.nCol;      /* hpplite_upd's, the widest call */
        if (nArgs > argLimit) {
          if (!textOnlyHas(pCtx->zRowsTextOnly, t.zName))
            fprintf(stderr, "hpplite: table %s has %d columns, past this SQLite's function argument limit (%d)"
                    " for row capture; its writes are logged as text\n", t.zName, t.nCol, argLimit);
          rc = textOnlyAdd(&zSkip, &nSkip, t.zName);
        } else if (!t.nKey && !t.zRowid) {
          if (!textOnlyHas(pCtx->zRowsTextOnly, t.zName))
            fprintf(stderr, "hpplite: table %s names columns rowid, _rowid_ and oid, leaving no name for its rows'"
                    " rowid; its writes are logged as text\n", t.zName);
          rc = textOnlyAdd(&zSkip, &nSkip, t.zName);
        } else {
          rc = buildTriggers(pCtx, &t, i);
        }
      }
      tabFree(&t);
    }
    sqlite3_free(azName[i]);
  }
  sqlite3_free(azName);
  if (rc == SQLITE_OK) rc = loadTextOnly(pCtx, zSkip);
  sqlite3_free(zSkip);
  pCtx->rowsBuiltFor = rc == SQLITE_OK ? v : -1;
  pCtx->rowsTempBuiltFor = tempSchemaVersionOf(pCtx);
  if (rc == SQLITE_OK) pCtx->bRebuildOwed = 0;
  hppliteInternalEnd(pCtx, saved);
  return rc;
}

static int addColumn(sqlite3 *db, const char *zCol, const char *zDecl) {
  sqlite3_stmt *q = NULL;
  int has = 0, rc = SQLITE_OK;
  char *z = sqlite3_mprintf("SELECT 1 FROM pragma_table_info('_hpplite_pending') WHERE name = %Q", zCol);
  if (sqlite3_prepare_v2(db, z, -1, &q, NULL) == SQLITE_OK) has = sqlite3_step(q) == SQLITE_ROW;
  sqlite3_finalize(q);
  sqlite3_free(z);
  if (!has) {
    z = sqlite3_mprintf("ALTER TABLE _hpplite_pending ADD COLUMN %s %s", zCol, zDecl);
    rc = sqlite3_exec(db, z, NULL, NULL, NULL);
    sqlite3_free(z);
  }
  return rc;
}

/* ------------------------------------------------------- sqlite_sequence */

static char *seqRead(sqlite3 *db) {
  sqlite3_stmt *q = NULL;
  sqlite3_str *s = sqlite3_str_new(NULL);
  if (sqlite3_prepare_v2(db, "SELECT name, seq FROM main.sqlite_sequence ORDER BY name", -1, &q, NULL) == SQLITE_OK) {
    while (sqlite3_step(q) == SQLITE_ROW) {
      sqlite3_str_appendall(s, (const char *)sqlite3_column_text(q, 0));
      sqlite3_str_appendchar(s, 1, 0);
      sqlite3_str_appendf(s, "%lld", sqlite3_column_int64(q, 1));
      sqlite3_str_appendchar(s, 1, 0);
    }
  }
  sqlite3_finalize(q);
  sqlite3_str_appendchar(s, 1, 0);
  return sqlite3_str_finish(s);
}

/* the value of zName in a snapshot, or NULL */
static const char *seqFind(const char *zSnap, const char *zName) {
  const char *z = zSnap;
  while (z && *z) {
    const char *v = z + strlen(z) + 1;
    if (strcmp(z, zName) == 0) return v;
    z = v + strlen(v) + 1;
  }
  return NULL;
}

char *hpplite_rows_seq_changed(HppliteCtx *pCtx, sqlite3 *db, int *pn, char **pzNow) {
  char *zNow = seqRead(db), *zOut;
  const char *z;
  sqlite3_str *s = sqlite3_str_new(NULL);
  *pn = 0;
  for (z = zNow; z && *z; ) {
    const char *v = z + strlen(z) + 1, *was = seqFind(pCtx->zRowsSeq, z);
    if (!was || strcmp(was, v) != 0) {
      sqlite3_str_appendall(s, z); sqlite3_str_appendchar(s, 1, 0);
      sqlite3_str_appendall(s, v); sqlite3_str_appendchar(s, 1, 0);
      (*pn)++;
    }
    z = v + strlen(v) + 1;
  }
  sqlite3_str_appendchar(s, 1, 0);
  if (pzNow) *pzNow = zNow; else sqlite3_free(zNow);
  zOut = sqlite3_str_finish(s);
  return zOut;
}

void hpplite_rows_seq_install(HppliteCtx *pCtx, char *zSnap) {
  sqlite3_free(pCtx->zRowsSeq);
  pCtx->zRowsSeq = zSnap;
}

void hpplite_rows_seq_take(HppliteCtx *pCtx, sqlite3 *db) {
  sqlite3_free(pCtx->zRowsSeq);
  pCtx->zRowsSeq = seqRead(db);
}

unsigned char *hpplite_rows_seq_rec(sqlite3_int64 seq, int *pnRec) {
  Rec r = {0};
  recVarint(&r, 1);
  recByte(&r, 1);
  recVarint(&r, ((sqlite3_uint64)seq << 1) ^ (sqlite3_uint64)(seq >> 63));
  if (r.oom) { sqlite3_free(r.a); return NULL; }
  *pnRec = r.n;
  return r.a;
}

int hpplite_rows_enable(HppliteCtx *pCtx) {
  sqlite3 *db = pCtx->db;
  int rc, saved = hppliteInternalBegin(pCtx);
  rc = addColumn(db, "tbl", "TEXT");
  if (rc == SQLITE_OK) rc = addColumn(db, "op", "INTEGER NOT NULL DEFAULT 0");
  if (rc == SQLITE_OK) rc = addColumn(db, "rec", "BLOB");
  hppliteInternalEnd(pCtx, saved);
  if (rc != SQLITE_OK) return rc;
  sqlite3_create_function(db, "hpplite_rec", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fnRec, NULL, NULL);
  sqlite3_create_function(db, "hpplite_upd", -1, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fnUpd, NULL, NULL);
  sqlite3_create_function(db, "hpplite_cap", 0, SQLITE_UTF8, pCtx, fnCap, NULL, NULL);
  sqlite3_create_function(db, "hpplite_grp", 0, SQLITE_UTF8, pCtx, fnGrp, NULL, NULL);
  sqlite3_create_function(db, "hpplite_note", 1, SQLITE_UTF8, pCtx, fnNote, NULL, NULL);
  sqlite3_create_function(db, "hpplite_mode", 0, SQLITE_UTF8, pCtx, fnMode, NULL, NULL);
  pCtx->bRows = 1;
  hpplite_rows_seq_take(pCtx, db);
  rc = rebuild(pCtx);
  /* the columns added above are a schema change of HPPLite's own: not one
  ** the connection's first statement is stale for */
  pCtx->seenSchema = schemaVersionOf(pCtx);
  return rc;
}

/* The connection as it was before hpplite_rows_enable(): no triggers, no
** functions - they hold pCtx, which hpplite_shutdown() frees while the
** connection lives on (#66) */
void hpplite_rows_disable(HppliteCtx *pCtx) {
  static const char *aFn[] = { "hpplite_rec", "hpplite_upd", "hpplite_cap", "hpplite_grp", "hpplite_note", "hpplite_mode" };
  static const int aArg[] = { -1, -1, 0, 0, 1, 0 };
  int i, saved;
  if (!pCtx->bRows) return;
  saved = hppliteInternalBegin(pCtx);
  dropAllTriggers(pCtx);
  for (i = 0; i < 6; i++) sqlite3_create_function(pCtx->db, aFn[i], aArg[i], SQLITE_UTF8, NULL, NULL, NULL, NULL);
  pCtx->bRows = 0;
  pCtx->rowsBuiltFor = -1;
  hppliteInternalEnd(pCtx, saved);
}

int hpplite_rows_covered(HppliteCtx *pCtx) {
  return pCtx->rowsBuiltFor >= 0 && schemaVersionOf(pCtx) == pCtx->rowsBuiltFor
      && tempSchemaVersionOf(pCtx) == pCtx->rowsTempBuiltFor;
}

/* The same, as a write starts. Read first, the schema version would open a
** snapshot the write then has to upgrade from - SQLITE_BUSY at once (no busy
** handler) when another connection committed in between. So the write lock
** comes first, with a write that changes nothing (in the statement's own
** transaction: it is running); held, it also keeps the schema as read until
** the statement ends. */
int hpplite_rows_covered_for_write(HppliteCtx *pCtx) {
  if (pCtx->rowsBuiltFor < 0) return 0;
  if (sqlite3_txn_state(pCtx->db, "main") != SQLITE_TXN_WRITE) {
    int rc, saved = hppliteInternalBegin(pCtx);
    /* The write that changes nothing: a pragma that begins a write
    ** transaction (the lock, the schema cookie check) and does nothing on a
    ** database not in incremental auto-vacuum mode. Not an UPDATE: a zero-row
    ** UPDATE sets the connection's change count to 0, and changes() inside
    ** the statement about to run must still say the previous statement's
    ** count, as on stock SQLite (#121) */
    if (!pCtx->pRowsLockStmt
        && sqlite3_prepare_v3(pCtx->db, "PRAGMA main.incremental_vacuum(0)", -1,
                              SQLITE_PREPARE_PERSISTENT, &pCtx->pRowsLockStmt, NULL) != SQLITE_OK) {
      pCtx->pRowsLockStmt = NULL;
    }
    rc = pCtx->pRowsLockStmt ? hpplite_step_write(pCtx->pRowsLockStmt) : SQLITE_ERROR;
    if (pCtx->pRowsLockStmt) sqlite3_reset(pCtx->pRowsLockStmt);
    hppliteInternalEnd(pCtx, saved);
    if ((rc & 0xff) == SQLITE_BUSY) return HPPLITE_ROWS_BUSY;   /* held elsewhere past the timeout (#92) */
    if ((rc & 0xff) == SQLITE_SCHEMA) return HPPLITE_ROWS_STALE; /* the schema stale past every reprepare (#108) */
    if (rc != SQLITE_DONE) return 0;      /* no lock for another reason: text, as v1 - its own insert says why */
  }
  return schemaVersionOf(pCtx) == pCtx->rowsBuiltFor && tempSchemaVersionOf(pCtx) == pCtx->rowsTempBuiltFor;
}

/* ------------------------------------------------- what a statement writes */

static const char *skipSpace(const char *z) {
  for (;;) {
    while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r' || *z == '\f') z++;
    if (z[0] == '-' && z[1] == '-') { while (*z && *z != '\n') z++; continue; }
    if (z[0] == '/' && z[1] == '*') { const char *e = strstr(z + 2, "*/"); z = e ? e + 2 : z + strlen(z); continue; }
    return z;
  }
}
static int isIdChar(unsigned char c) { return isalnum(c) || c == '_' || c == '$' || c >= 0x80; }
static int isWord(const char *z, const char *zWord) {
  int n = (int)strlen(zWord);
  return sqlite3_strnicmp(z, zWord, n) == 0 && !isIdChar((unsigned char)z[n]);
}
/* past one token: a quoted string or name, a word, or one character */
static const char *skipToken(const char *z) {
  char close = 0;
  if (*z == '\'' || *z == '"' || *z == '`') close = *z;
  else if (*z == '[') close = ']';
  if (close) {
    for (z++; *z; z++) {
      if (*z == close) { if (close != ']' && z[1] == close) { z++; continue; } return z + 1; }
    }
    return z;
  }
  if (isIdChar((unsigned char)*z)) { while (isIdChar((unsigned char)*z)) z++; return z; }
  return *z ? z + 1 : z;
}
/* a name (quoted or not) into zOut, unquoted; the position after it. One
** too long for zOut is no name (empty): compared cut, a virtual table's
** could pass for another's (#78) */
static const char *readName(const char *z, char *zOut, int nOut) {
  const char *e = skipToken(z);
  int n = 0, cut = 0;
  if (*z == '"' || *z == '`' || *z == '[') {
    char close = *z == '[' ? ']' : *z;
    for (z++; z < e - 1; z++) {
      if (*z == close && close != ']' && z[1] == close) z++;
      if (n < nOut - 1) zOut[n++] = *z; else cut = 1;
    }
  } else if (isIdChar((unsigned char)*z)) {
    for (; z < e; z++) { if (n < nOut - 1) zOut[n++] = *z; else cut = 1; }
  }
  zOut[cut ? 0 : n] = 0;
  return e;
}

/* The table (or view) an INSERT, REPLACE, UPDATE or DELETE writes, WITH in
** front or not; 0 when the text does not say plainly */
static int writeTarget(const char *z, char *zOut, int nOut) {
  z = skipSpace(z);
  if (isWord(z, "WITH")) {
    int depth = 0;
    for (z = skipSpace(z + 4); *z; z = skipSpace(skipToken(z))) {
      if (*z == '(') depth++;
      else if (*z == ')') depth--;
      else if (depth == 0 && (isWord(z, "INSERT") || isWord(z, "REPLACE") || isWord(z, "UPDATE") || isWord(z, "DELETE"))) break;
    }
  }
  if (isWord(z, "INSERT")) {
    z = skipSpace(z + 6);
    if (isWord(z, "OR")) z = skipSpace(skipToken(skipSpace(z + 2)));
    if (!isWord(z, "INTO")) return 0;
    z = skipSpace(z + 4);
  } else if (isWord(z, "REPLACE")) {
    z = skipSpace(z + 7);
    if (!isWord(z, "INTO")) return 0;
    z = skipSpace(z + 4);
  } else if (isWord(z, "UPDATE")) {
    z = skipSpace(z + 6);
    if (isWord(z, "OR")) z = skipSpace(skipToken(skipSpace(z + 2)));
  } else if (isWord(z, "DELETE")) {
    z = skipSpace(z + 6);
    if (!isWord(z, "FROM")) return 0;
    z = skipSpace(z + 4);
  } else {
    return 0;
  }
  z = skipSpace(readName(z, zOut, nOut));
  if (*z == '.') readName(skipSpace(z + 1), zOut, nOut);    /* schema.name: the name */
  return zOut[0] != 0;
}

/* ALTER TABLE [schema.]name DROP [COLUMN] ...: the table, else 0. SQLite
** checks every trigger on the table after it drops a column, ours too: one
** that names the column makes the ALTER fail ("error in trigger ... after
** drop column"). RENAME and ADD need nothing: SQLite rewrites TEMP triggers. */
int hpplite_rows_drop_column_target(const char *z, char *zOut, int nOut) {
  z = skipSpace(z);
  if (!isWord(z, "ALTER")) return 0;
  z = skipSpace(z + 5);
  if (!isWord(z, "TABLE")) return 0;
  z = skipSpace(readName(skipSpace(z + 5), zOut, nOut));
  if (*z == '.') z = skipSpace(readName(skipSpace(z + 1), zOut, nOut));
  return zOut[0] && isWord(z, "DROP");
}

/* Drop the capture triggers of one table (they are built again after the
** statement: the schema moved) */
int hpplite_rows_drop_triggers_for(HppliteCtx *pCtx, const char *zTab) {
  sqlite3_stmt *q = NULL;
  char *zDrop = NULL;
  int rc, saved = hppliteInternalBegin(pCtx);
  rc = sqlite3_prepare_v2(pCtx->db, "SELECT name FROM temp.sqlite_schema WHERE type = 'trigger'"
                          " AND name LIKE 'hpplite\\_cap\\_%' ESCAPE '\\' AND tbl_name = ?1 COLLATE NOCASE", -1, &q, NULL);
  if (rc == SQLITE_OK) {
    sqlite3_bind_text(q, 1, zTab, -1, SQLITE_STATIC);
    while (sqlite3_step(q) == SQLITE_ROW)
      zDrop = sqlite3_mprintf("%zDROP TRIGGER temp.\"%w\";", zDrop, (const char *)sqlite3_column_text(q, 0));
  }
  sqlite3_finalize(q);
  if (rc == SQLITE_OK && zDrop) rc = sqlite3_exec(pCtx->db, zDrop, NULL, NULL, NULL);
  sqlite3_free(zDrop);
  pCtx->rowsBuiltFor = -1;
  hppliteInternalEnd(pCtx, saved);
  return rc;
}

/* SQLite's own tables carry no triggers (sqlite_schema under
** writable_schema, sqlite_stat1): a write to one is text (#74) */
static int isSqliteTable(const char *zName) { return sqlite3_strnicmp(zName, "sqlite_", 7) == 0; }
static int mentionsSqliteTable(const char *zSql) {
  const char *z;
  for (z = zSql; *z; z++) {
    if (!isSqliteTable(z)) continue;
    if (z > zSql && isIdChar((unsigned char)z[-1])) continue;
    return 1;
  }
  return 0;
}

/* UPDATE [OR ...] [schema.]sqlite_schema ... or DELETE FROM [schema.]sqlite_schema
** ... with no WHERE at the top level (#124): under writable_schema it
** rewrites every schema row, HPPLite's with the application's, and the
** authorizer cannot see which rows a statement touches. A WHERE inside a
** subquery or parentheses does not count. */
int hpplite_sql_whole_schema_write(const char *zSql) {
  static const char *aSchema[] = { "sqlite_schema", "sqlite_master", "sqlite_temp_schema", "sqlite_temp_master", NULL };
  char name[64];
  const char *z = skipSpace(zSql);
  int depth = 0, i, isSchema = 0;
  if (isWord(z, "UPDATE")) {
    z = skipSpace(z + 6);
    if (isWord(z, "OR")) z = skipSpace(skipToken(skipSpace(z + 2)));   /* OR ROLLBACK / ABORT / REPLACE / FAIL / IGNORE */
  } else if (isWord(z, "DELETE")) {
    z = skipSpace(z + 6);
    if (!isWord(z, "FROM")) return 0;
    z = skipSpace(z + 4);
  } else {
    return 0;
  }
  z = readName(z, name, sizeof name);
  if (*skipSpace(z) == '.') z = readName(skipSpace(skipSpace(z) + 1), name, sizeof name);   /* schema.name */
  for (i = 0; aSchema[i]; i++) if (sqlite3_stricmp(name, aSchema[i]) == 0) isSchema = 1;
  if (!isSchema) return 0;
  for (z = skipSpace(z); *z; z = skipSpace(skipToken(z))) {
    if (*z == '(') { depth++; z++; continue; }
    if (*z == ')') { if (depth > 0) depth--; z++; continue; }
    if (depth == 0 && isWord(z, "WHERE")) return 0;
  }
  return 1;
}

/* A write reaches a virtual table when the table it writes is one, or has
** triggers or foreign key actions that reach one. Text that does not say
** plainly what it writes is judged by every name it mentions. */
int hpplite_rows_text_only(HppliteCtx *pCtx, const char *zSql) {
  char zTarget[256];
  if (writeTarget(zSql, zTarget, (int)sizeof(zTarget)))
    return isSqliteTable(zTarget) || (pCtx->zRowsTextOnly && textOnlyHas(pCtx->zRowsTextOnly, zTarget));
  return mentionsSqliteTable(zSql) || (pCtx->zRowsTextOnly && textOnlyMentioned(pCtx->zRowsTextOnly, zSql));
}

void hpplite_rows_after_statement(HppliteCtx *pCtx) {
  if (!pCtx->bRows || hpplite_rows_covered(pCtx)) return;
  rebuild(pCtx);     /* on failure (a statement still holding the schema) the next one is text */
}

/* ------------------------------------------------------------------ replay */

typedef struct Dec { const unsigned char *a; int n, i, bad; } Dec;
static sqlite3_uint64 decVarint(Dec *d) {
  sqlite3_uint64 v = 0;
  int shift = 0;
  while (d->i < d->n && shift < 64) {
    unsigned char b = d->a[d->i++];
    v |= (sqlite3_uint64)(b & 0x7f) << shift;
    if (!(b & 0x80)) return v;
    shift += 7;
  }
  d->bad = 1;
  return 0;
}
static void decBind(Dec *d, sqlite3_stmt *st, int iParam) {
  unsigned char tag;
  if (d->i >= d->n) { d->bad = 1; return; }
  tag = d->a[d->i++];
  switch (tag) {
    case 0: sqlite3_bind_null(st, iParam); break;
    case 1: {
      sqlite3_uint64 u = decVarint(d);
      sqlite3_bind_int64(st, iParam, (sqlite3_int64)((u >> 1) ^ (~(u & 1) + 1)));
      break;
    }
    case 2: {
      sqlite3_uint64 u = 0;
      double x;
      int k;
      if (d->i + 8 > d->n) { d->bad = 1; return; }
      for (k = 0; k < 8; k++) u = (u << 8) | d->a[d->i++];
      memcpy(&x, &u, 8);
      sqlite3_bind_double(st, iParam, x);
      break;
    }
    case 3: case 4: {
      sqlite3_uint64 len = decVarint(d);
      if (d->bad || len > (sqlite3_uint64)(d->n - d->i)) { d->bad = 1; return; }
      if (tag == 3) sqlite3_bind_text(st, iParam, (const char *)d->a + d->i, (int)len, SQLITE_TRANSIENT);
      else sqlite3_bind_blob(st, iParam, d->a + d->i, (int)len, SQLITE_TRANSIENT);
      d->i += (int)len;
      break;
    }
    default: d->bad = 1;
  }
}
/* skip one value (the key of an insert into a rowid table with an INTEGER PRIMARY KEY is that column) */
static void decSkip(Dec *d) {
  unsigned char tag;
  if (d->i >= d->n) { d->bad = 1; return; }
  tag = d->a[d->i++];
  if (tag == 1) decVarint(d);
  else if (tag == 2) d->i += 8;
  else if (tag == 3 || tag == 4) {
    sqlite3_uint64 len = decVarint(d);                 /* bounded first: a length past int would wrap the cursor (#69) */
    if (d->bad || len > (sqlite3_uint64)(d->n - d->i)) { d->bad = 1; return; }
    d->i += (int)len;
  }
  if (d->i > d->n) d->bad = 1;
}

static char *whereKey(const Tab *t, int firstParam) {
  char *z = NULL;
  int i;
  if (t->nKey == 0) return sqlite3_mprintf("%s = ?%d", t->zRowid, firstParam);
  for (i = 0; i < t->nKey; i++) z = sqlite3_mprintf("%z%s\"%w\" = ?%d", z, i ? " AND " : "", t->azKey[i], firstParam + i);
  return z;
}

int hpplite_rows_apply(sqlite3 *db, const char *zTab, int op, const unsigned char *aRec, int nRec, char **pzErr) {
  Tab t;
  Dec d = { aRec, nRec, 0, 0 };
  sqlite3_stmt *st = NULL;
  char *zSql = NULL;
  int rc, i, n;
  *pzErr = NULL;
  if (op == HPPLITE_ROW_SEQ) {
    /* set exactly: the writer's value at this point of the stream */
    if (decVarint(&d) != 1) { *pzErr = sqlite3_mprintf("sequence record for %s: malformed", zTab); return SQLITE_CORRUPT; }
    rc = sqlite3_prepare_v2(db, "UPDATE main.sqlite_sequence SET seq = ?1 WHERE name = ?2", -1, &st, NULL);
    if (rc == SQLITE_OK) {
      decBind(&d, st, 1);
      sqlite3_bind_text(st, 2, zTab, -1, SQLITE_STATIC);
      rc = hpplite_step_write(st) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
      sqlite3_finalize(st);
      st = NULL;
    }
    if (rc == SQLITE_OK && sqlite3_changes(db) == 0) {
      d.i = 0; decVarint(&d);
      rc = sqlite3_prepare_v2(db, "INSERT INTO main.sqlite_sequence(name, seq) VALUES(?2, ?1)", -1, &st, NULL);
      if (rc == SQLITE_OK) {
        decBind(&d, st, 1);
        sqlite3_bind_text(st, 2, zTab, -1, SQLITE_STATIC);
        rc = hpplite_step_write(st) == SQLITE_DONE ? SQLITE_OK : SQLITE_ERROR;
        sqlite3_finalize(st);
      }
    }
    if (rc != SQLITE_OK || d.bad) {
      *pzErr = sqlite3_mprintf("sequence record for %s: %s", zTab, d.bad ? "malformed" : sqlite3_errmsg(db));
      return d.bad ? SQLITE_CORRUPT : rc;
    }
    return SQLITE_OK;
  }
  rc = tabLoad(db, zTab, &t);
  if (rc != SQLITE_OK || t.nCol == 0) {
    *pzErr = sqlite3_mprintf("record for table %s, which the replica does not have", zTab);
    tabFree(&t);
    return SQLITE_ERROR;
  }
  if (!t.nKey && !t.zRowid) {
    *pzErr = sqlite3_mprintf("record for table %s, whose rows have no name for their rowid (the writer logs its writes as text)", zTab);
    tabFree(&t);
    return SQLITE_ERROR;
  }
  n = (int)decVarint(&d);
  if (op == HPPLITE_ROW_INSERT) {
    /* INSERT OR REPLACE with the writer's key: a REPLACE conflict on the
    ** writer replaces the same rows here */
    char *zCols = NULL, *zVals = NULL;
    int nWant = (t.nKey ? 0 : 1) + t.nCol, p = 1;
    if (n != nWant) { *pzErr = sqlite3_mprintf("insert record for %s: %d values, table has %d", zTab, n, nWant); tabFree(&t); return SQLITE_ERROR; }
    if (!t.nKey && t.ipk < 0) { zCols = sqlite3_mprintf("%s", t.zRowid); zVals = sqlite3_mprintf("?%d", p++); }
    for (i = 0; i < t.nCol; i++) {
      zCols = sqlite3_mprintf("%z%s\"%w\"", zCols, zCols ? ", " : "", t.azCol[i]);
      zVals = sqlite3_mprintf("%z%s?%d", zVals, zVals ? ", " : "", p++);
    }
    zSql = sqlite3_mprintf("INSERT OR REPLACE INTO main.\"%w\"(%s) VALUES(%s)", zTab, zCols, zVals);
    sqlite3_free(zCols); sqlite3_free(zVals);
    rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
    if (rc == SQLITE_OK) {
      p = 1;
      if (!t.nKey) { if (t.ipk < 0) decBind(&d, st, p++); else decSkip(&d); }
      for (i = 0; i < t.nCol && !d.bad; i++) decBind(&d, st, p++);
    }
  } else if (op == HPPLITE_ROW_DELETE) {
    int nKey = t.nKey ? t.nKey : 1;
    char *zWhere = whereKey(&t, 1);
    if (n != nKey) { *pzErr = sqlite3_mprintf("delete record for %s: key of %d values, table's has %d", zTab, n, nKey); sqlite3_free(zWhere); tabFree(&t); return SQLITE_ERROR; }
    zSql = sqlite3_mprintf("DELETE FROM main.\"%w\" WHERE %s", zTab, zWhere);
    sqlite3_free(zWhere);
    rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
    for (i = 0; rc == SQLITE_OK && i < nKey && !d.bad; i++) decBind(&d, st, 1 + i);
  } else if (op == HPPLITE_ROW_UPDATE) {
    /* old key (?1..), then the new rowid if it moved, then the changed columns */
    int nKey = t.nKey ? t.nKey : 1, keyChanged, nCol, at, p;
    char *zSet = NULL, *zWhere;
    const unsigned char *bitmap;
    if (n != nKey) { *pzErr = sqlite3_mprintf("update record for %s: key of %d values, table's has %d", zTab, n, nKey); tabFree(&t); return SQLITE_ERROR; }
    at = d.i;                                         /* old key values: bound after the statement exists */
    for (i = 0; i < nKey; i++) decSkip(&d);
    keyChanged = d.i < d.n ? d.a[d.i++] : (d.bad = 1, 0);
    {
      int newKeyAt = d.i;
      if (keyChanged) for (i = 0; i < nKey; i++) decSkip(&d);
      nCol = (int)decVarint(&d);
      if (d.bad || nCol != t.nCol || d.i + (nCol + 7) / 8 > d.n) {
        *pzErr = sqlite3_mprintf("update record for %s: %d columns, table has %d", zTab, nCol, t.nCol);
        tabFree(&t);
        return SQLITE_ERROR;
      }
      bitmap = d.a + d.i;
      d.i += (nCol + 7) / 8;
      p = nKey + 1;
      /* a rowid table's key moving is a column (its INTEGER PRIMARY KEY, in
      ** the bitmap) or the bare rowid */
      if (keyChanged && !t.nKey && t.ipk < 0) zSet = sqlite3_mprintf("%s = ?%d", t.zRowid, p++);
      for (i = 0; i < nCol; i++) if (bitmap[i / 8] & (1 << (i % 8))) zSet = sqlite3_mprintf("%z%s\"%w\" = ?%d", zSet, zSet ? ", " : "", t.azCol[i], p++);
      if (!zSet) { tabFree(&t); return SQLITE_OK; }   /* nothing changed */
      zWhere = whereKey(&t, 1);
      /* OR REPLACE as the insert: a REPLACE conflict on the writer removed
      ** the rows in the way without a delete trigger firing (recursive
      ** triggers off), so no record says so; here it removes the same (#64) */
      zSql = sqlite3_mprintf("UPDATE OR REPLACE main.\"%w\" SET %s WHERE %s", zTab, zSet, zWhere);
      sqlite3_free(zSet); sqlite3_free(zWhere);
      rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
      if (rc == SQLITE_OK) {
        Dec k = { aRec, nRec, at, 0 };
        for (i = 0; i < nKey; i++) decBind(&k, st, 1 + i);
        p = nKey + 1;
        k.i = newKeyAt;
        if (keyChanged && !t.nKey && t.ipk < 0) decBind(&k, st, p++);
        for (i = 0; i < nCol && !d.bad; i++) if (bitmap[i / 8] & (1 << (i % 8))) decBind(&d, st, p++);
        if (k.bad) d.bad = 1;
      }
    }
  } else {
    *pzErr = sqlite3_mprintf("unknown record op %d", op);
    tabFree(&t);
    return SQLITE_ERROR;
  }
  if (rc == SQLITE_OK && d.bad) { *pzErr = sqlite3_mprintf("malformed record for %s", zTab); rc = SQLITE_CORRUPT; }
  if (rc == SQLITE_OK) {
    rc = hpplite_step_write(st);
    rc = rc == SQLITE_DONE ? SQLITE_OK : rc;
    if (rc != SQLITE_OK) *pzErr = sqlite3_mprintf("%s: %s", zSql, sqlite3_errmsg(db));
  } else if (!*pzErr) {
    *pzErr = sqlite3_mprintf("%s: %s", zSql ? zSql : zTab, sqlite3_errmsg(db));
  }
  sqlite3_finalize(st);
  sqlite3_free(zSql);
  tabFree(&t);
  return rc;
}

/* ----------------------------------------------------------- batch entries */

/* A record as a batch entry: base64 of (op, table, record), so it is text
** like a statement and is cut into pieces (#59) and held open (#57) the
** same way */
static const char aB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *hpplite_rows_entry(int op, const char *zTab, const unsigned char *aRec, int nRec) {
  Rec r = {0};
  int nTab = zTab ? (int)strlen(zTab) : 0, i, j = 0;
  char *z;
  recVarint(&r, (sqlite3_uint64)op);
  recBytes(&r, zTab ? zTab : "", nTab);
  recGrow(&r, nRec);
  if (!r.oom && nRec > 0) { memcpy(r.a + r.n, aRec, (size_t)nRec); r.n += nRec; }
  if (r.oom) { sqlite3_free(r.a); return NULL; }
  z = sqlite3_malloc64((sqlite3_uint64)(r.n + 2) / 3 * 4 + 1);
  if (z) {
    for (i = 0; i + 2 < r.n; i += 3) {
      unsigned v = (unsigned)r.a[i] << 16 | (unsigned)r.a[i + 1] << 8 | r.a[i + 2];
      z[j++] = aB64[v >> 18]; z[j++] = aB64[(v >> 12) & 63]; z[j++] = aB64[(v >> 6) & 63]; z[j++] = aB64[v & 63];
    }
    if (i < r.n) {
      unsigned v = (unsigned)r.a[i] << 16 | (i + 1 < r.n ? (unsigned)r.a[i + 1] << 8 : 0);
      z[j++] = aB64[v >> 18]; z[j++] = aB64[(v >> 12) & 63];
      z[j++] = i + 1 < r.n ? aB64[(v >> 6) & 63] : '=';
      z[j++] = '=';
    }
    z[j] = 0;
  }
  sqlite3_free(r.a);
  return z;
}

static int b64val(int c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* An entry's bytes: op, table, and where its record starts */
static int entryDecode(const char *zEntry, unsigned char **paOut, int *pnOut, int *pOp, char **pzTab, int *piRec) {
  size_t n = strlen(zEntry), i;
  unsigned char *a = sqlite3_malloc64(n / 4 * 3 + 3);
  int na = 0;
  unsigned v = 0, bits = 0;
  Dec d;
  sqlite3_uint64 op, nTab;
  if (!a) return SQLITE_NOMEM;
  for (i = 0; i < n && zEntry[i] != '='; i++) {
    int x = b64val((unsigned char)zEntry[i]);
    if (x < 0) { sqlite3_free(a); return SQLITE_CORRUPT; }
    v = v << 6 | (unsigned)x;
    bits += 6;
    if (bits >= 8) { bits -= 8; a[na++] = (unsigned char)(v >> bits); }
  }
  d.a = a; d.n = na; d.i = 0; d.bad = 0;
  op = decVarint(&d);
  nTab = decVarint(&d);
  if (d.bad || nTab > (sqlite3_uint64)(d.n - d.i)) { sqlite3_free(a); return SQLITE_CORRUPT; }
  *pzTab = sqlite3_mprintf("%.*s", (int)nTab, (const char *)a + d.i);
  if (!*pzTab) { sqlite3_free(a); return SQLITE_NOMEM; }
  *pOp = (int)op;
  *piRec = d.i + (int)nTab;
  *paOut = a;
  *pnOut = na;
  return SQLITE_OK;
}

int hpplite_rows_entry_info(const char *zEntry, int *pOp, char **pzTab) {
  unsigned char *a = NULL;
  int n, iRec, rc = entryDecode(zEntry, &a, &n, pOp, pzTab, &iRec);
  sqlite3_free(a);
  return rc;
}

int hpplite_rows_apply_entry(sqlite3 *db, const char *zEntry, char **pzErr) {
  unsigned char *a = NULL;
  char *zTab = NULL;
  int n = 0, op = 0, iRec = 0, rc;
  *pzErr = NULL;
  rc = entryDecode(zEntry, &a, &n, &op, &zTab, &iRec);
  if (rc != SQLITE_OK) {
    *pzErr = sqlite3_mprintf("row entry: %s", rc == SQLITE_CORRUPT ? "not a record" : "out of memory");
    return rc;
  }
  rc = hpplite_rows_apply(db, zTab, op, a + iRec, n - iRec, pzErr);
  sqlite3_free(zTab);
  sqlite3_free(a);
  return rc;
}
