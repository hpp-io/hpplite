/*
** HPPLite - capture: the hooks on the application connection.
**
** The statement trace (sqlite3_trace_v2) logs each write into
** _hpplite_pending from inside the write's own transaction, or refuses it;
** the authorizer keeps HPPLite's tables and the connection's pragmas in
** view; the commit and rollback hooks keep the counters. Row capture (the
** TEMP triggers) is in capture_rows.c; this file decides which statements
** are logged as text and runs the machinery around a statement SQLite reruns
** on a stale schema.
*/

#include "hpplite_int.h"
#include <ctype.h>
#include <fnmatch.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

/*
** Connection pragmas that change what a statement does without showing in
** its text (#28): legacy_alter_table, writable_schema,
** ignore_check_constraints, case_sensitive_like. Their state is kept from
** the application's own PRAGMA statements (the authorizer sees each one)
** and read once at init for the three that can be read; case_sensitive_like
** has no query form. Each logged statement and record carries the bits, and
** replay sets them before it, as it does foreign keys (#52).
*/
static int pragmaBoolean(const char *z) {
  while (*z == ' ' || *z == '\t') z++;
  if ((*z >= '0' && *z <= '9') || *z == '-') return atoi(z) != 0;
  return sqlite3_strnicmp(z, "on", 2) == 0 || sqlite3_strnicmp(z, "yes", 3) == 0 || sqlite3_strnicmp(z, "true", 4) == 0;
}
static int pragmaModeBit(const char *zName) {
  if (sqlite3_stricmp(zName, "legacy_alter_table") == 0) return HPPLITE_MODE_LEGACY_ALTER;
  if (sqlite3_stricmp(zName, "writable_schema") == 0) return HPPLITE_MODE_WRITABLE_SCHEMA;
  if (sqlite3_stricmp(zName, "ignore_check_constraints") == 0) return HPPLITE_MODE_IGNORE_CHECKS;
  if (sqlite3_stricmp(zName, "case_sensitive_like") == 0) return HPPLITE_MODE_CASE_LIKE;
  return 0;
}
static void pragmaModeSet(HppliteCtx *pCtx, const char *zName, const char *zValue) {
  int bit = pragmaModeBit(zName);
  if (!bit) return;
  /* writable_schema=RESET turns it off (and reloads the schema) */
  if (pragmaBoolean(zValue)) pCtx->pragmaMode |= bit; else pCtx->pragmaMode &= ~bit;
}
void hpplitePragmaModeRead(HppliteCtx *pCtx) {
  static const char *aRead[] = { "legacy_alter_table", "writable_schema", "ignore_check_constraints", NULL };
  int i, saved = hppliteInternalBegin(pCtx);
  for (i = 0; aRead[i]; i++) {
    sqlite3_stmt *q = NULL;
    char *zSql = sqlite3_mprintf("PRAGMA %s", aRead[i]);
    if (zSql && sqlite3_prepare_v2(pCtx->db, zSql, -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) {
      if (sqlite3_column_int(q, 0)) pCtx->pragmaMode |= pragmaModeBit(aRead[i]);
      else pCtx->pragmaMode &= ~pragmaModeBit(aRead[i]);
    }
    sqlite3_finalize(q);
    sqlite3_free(zSql);
  }
  hppliteInternalEnd(pCtx, saved);
}

/* PRAGMA [schema.]user_version / application_id = N (or (N)): the two header
** fields an application writes - migration tools keep their version there.
** Deterministic, so captured and replayed like any write; their reads are not. */
static int isHeaderPragmaWrite(const char *z) {
  /* schema_version: set by hand after editing sqlite_schema under
  ** writable_schema, so every connection reloads the schema (#28) */
  static const char *aName[] = { "user_version", "application_id", "schema_version", NULL };
  int i;
  while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
  if (sqlite3_strnicmp(z, "pragma", 6) != 0) return 0;
  z += 6;
  while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
  {
    const char *dot = z;
    while (*dot && (isalnum((unsigned char)*dot) || *dot == '_')) dot++;
    if (*dot == '.') z = dot + 1;                          /* main.user_version */
  }
  for (i = 0; aName[i]; i++) {
    size_t n = strlen(aName[i]);
    if (sqlite3_strnicmp(z, aName[i], (int)n) == 0 && !(isalnum((unsigned char)z[n]) || z[n] == '_')) {
      z += n;
      while (*z == ' ' || *z == '\t') z++;
      return *z == '=' || *z == '(';
    }
  }
  return 0;
}

/* A write row capture records (INSERT, REPLACE, UPDATE, DELETE, and WITH
** in front of one); schema changes and the rest stay statement text */
static int isRowsDml(const char *z) {
  static const char *aDml[] = { "INSERT", "REPLACE", "UPDATE", "DELETE", "WITH", NULL };
  int i;
  for (;;) {
    while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
    if (z[0] == '-' && z[1] == '-') { while (*z && *z != '\n') z++; }
    else if (z[0] == '/' && z[1] == '*') { const char *e = strstr(z + 2, "*/"); if (!e) return 0; z = e + 2; }
    else break;
  }
  for (i = 0; aDml[i]; i++) {
    size_t n = strlen(aDml[i]);
    if (sqlite3_strnicmp(z, aDml[i], (int)n) == 0 && !((z[n] >= 'A' && z[n] <= 'Z') || (z[n] >= 'a' && z[n] <= 'z') || z[n] == '_')) return 1;
  }
  return 0;
}

/*
** Decide whether a statement is a write worth logging, by its first keyword.
*/
static int isTrackedSql(const char *z) {
  static const char *aSkip[] = {
    "SELECT", "EXPLAIN", "PRAGMA", "BEGIN", "COMMIT", "END", "ROLLBACK",
    "SAVEPOINT", "RELEASE", "VACUUM", "ANALYZE", "REINDEX", "ATTACH", "DETACH",
    NULL
  };
  int i;

  /* Skip whitespace and comments. Trigger sub-programs are traced as a bare
  ** "-- TRIGGER name" comment; they are reproduced by replaying the statement
  ** that fired them. */
  for (;;) {
    while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
    if (z[0] == '-' && z[1] == '-') {
      while (*z && *z != '\n') z++;
    } else if (z[0] == '/' && z[1] == '*') {
      const char *zEnd = strstr(z + 2, "*/");
      if (!zEnd) return 0;
      z = zEnd + 2;
    } else {
      break;
    }
  }
  if (*z == '\0') return 0;
  if (isHeaderPragmaWrite(z)) return 1;
  for (i = 0; aSkip[i]; i++) {
    int n = (int)strlen(aSkip[i]);
    if (sqlite3_strnicmp(z, aSkip[i], n) == 0) {
      char c = z[n];
      if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
        return 0;
      }
    }
  }
  return 1;
}

static sqlite3_int64 totalChanges(sqlite3 *db) {
#if SQLITE_VERSION_NUMBER >= 3037000
  return sqlite3_total_changes64(db);
#else
  return sqlite3_total_changes(db);
#endif
}

int hppliteSchemaVersion(HppliteCtx *pCtx) {
  int v = -1, saved = hppliteInternalBegin(pCtx);   /* ours: its trace must not pass for the application's next statement */
  if (!pCtx->pSchemaStmt) {
    if (sqlite3_prepare_v3(pCtx->db, "PRAGMA schema_version", -1, SQLITE_PREPARE_PERSISTENT,
                           &pCtx->pSchemaStmt, NULL) != SQLITE_OK) {
      pCtx->pSchemaStmt = NULL;
      hppliteInternalEnd(pCtx, saved);
      return -1;
    }
  }
  if (sqlite3_step(pCtx->pSchemaStmt) == SQLITE_ROW) v = sqlite3_column_int(pCtx->pSchemaStmt, 0);
  sqlite3_reset(pCtx->pSchemaStmt);
  hppliteInternalEnd(pCtx, saved);
  return v;
}

/*
** A logged statement finished. If it had no effect, remove its log row.
**
** A statement's error is not observable from the trace (the profile
** callback runs before SQLite records the result code), but its effect is:
** rows changed (sqlite3_total_changes, which excludes the rows of an
** aborted statement) or a schema change. A statement with neither either
** failed or was a no-op; replaying it adds nothing and, if it failed, would
** fail again - or succeed elsewhere and diverge.
**
** In autocommit a failed statement's implicit transaction already rolled
** the row back. Inside an explicit transaction a failed statement only
** undoes its own changes, and the log row was written by a separate
** statement, so it would survive to COMMIT. The DELETE runs inside the same
** transaction.
*/
static int seqFingerprint(HppliteCtx *pCtx, sqlite3_int64 out[2]);
/* Did the logged statement do anything: changes(), the schema version or
** sqlite_sequence moved since its row was written. Under bInternal. */
static int loggedHadEffect(HppliteCtx *pCtx) {
  sqlite3_int64 now[2];
  if (totalChanges(pCtx->db) != pCtx->logged.changes || hppliteSchemaVersion(pCtx) != pCtx->logged.schema) return 1;
  /* no row changed, but sqlite_sequence may have */
  return pCtx->logged.bSeq && seqFingerprint(pCtx, now) && (now[0] != pCtx->logged.seq[0] || now[1] != pCtx->logged.seq[1]);
}
/* The last logged row goes, inside the application's transaction. Under bInternal. */
static void unlogLast(HppliteCtx *pCtx) {
  if (!pCtx->pUnlogStmt) {
    if (sqlite3_prepare_v3(pCtx->db, "DELETE FROM _hpplite_pending WHERE id = ?1",
                           -1, SQLITE_PREPARE_PERSISTENT, &pCtx->pUnlogStmt, NULL) != SQLITE_OK) {
      pCtx->pUnlogStmt = NULL;
    }
  }
  if (!pCtx->pUnlogStmt) return;
  sqlite3_bind_int64(pCtx->pUnlogStmt, 1, pCtx->logged.id);
  hpplite_step_write(pCtx->pUnlogStmt);
  sqlite3_reset(pCtx->pUnlogStmt);
  /* changes() now says 1 (our DELETE); the application's statement changed
  ** nothing - that is why its row went. A zero-row UPDATE says 0 again. */
  if (!pCtx->pZeroStmt
      && sqlite3_prepare_v3(pCtx->db, "UPDATE _hpplite_pending SET txn = txn WHERE 0", -1,
                            SQLITE_PREPARE_PERSISTENT, &pCtx->pZeroStmt, NULL) != SQLITE_OK) {
    pCtx->pZeroStmt = NULL;
  }
  if (pCtx->pZeroStmt) {
    hpplite_step_write(pCtx->pZeroStmt);
    sqlite3_reset(pCtx->pZeroStmt);
  }
}
static void hppliteStatementDone(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  int saved;
  if (pS != pCtx->logged.pS) return;
  pCtx->logged.pS = NULL;
  if (pCtx->logged.keep) return;   /* a header pragma changes neither rows nor schema: keep */
  /* Reset before it finished (a RETURNING write stepped once - node:sqlite's
  ** .get()): SQLite reports the end from sqlite3_reset() while the statement
  ** is still busy, before it counts the changes it made. Its writes are done
  ** (they all happen in the first step), the count says nothing: keep. */
  if (sqlite3_stmt_busy(pS)) return;
  saved = hppliteInternalBegin(pCtx);
  /* Had an effect: keep. No effect: inside a transaction its row goes
  ** before anyone can see it - a statement that failed there must not reach
  ** the chain (in autocommit a failed statement's rollback took the row
  ** already), and a no-op need not. In autocommit the row committed with the
  ** statement and may already be in a snapshot - a batch being built in this
  ** process or another; removing it now would leave that batch carrying a
  ** row the log no longer has. It stays: a statement that changed nothing on
  ** the writer changes nothing on a replica with the same state. */
  if (!loggedHadEffect(pCtx) && !sqlite3_get_autocommit(pCtx->db)) unlogLast(pCtx);
  hppliteInternalEnd(pCtx, saved);
}

/*
** Replay re-executes SQL text, so a statement whose result depends on
** anything but the database (random values, the clock, the connection) is
** not reproducible: the sequencer's rows and a verifier's rows differ and
** the root check fails. Spot the usual suspects and count them, so a user
** can see why verification would fail (hpplite_status.nondeterministic).
*/
/* What a text rule finds against a statement (textVerdict) */
typedef enum {
  TEXT_REPLAYS = 0,  /* nothing against it */
  TEXT_VALUE,        /* a value replay cannot reproduce: counted; determinism=warn lets it through */
  TEXT_FUNCTION,     /* a function the replica does not have: counted, never let through */
  TEXT_NEVER         /* never, and not a determinism matter: the check failed, our tables named,
                     ** bound values this SQLite cuts (#62) */
} TextVerdict;

/*
** The things the authorizer cannot see: keywords (CURRENT_*), the zero-
** argument time functions, strftime with only a format, and the literal
** 'now' as a time argument. Scans outside string literals; inside one,
** only a lone 'now' (or 'localtime'-style modifier after it) counts, and
** only in argument position: due='now' is data, datetime('now') is the clock.
*/
/* Is position i (a literal's opening quote) an argument of date(), time(),
** datetime(), julianday(), unixepoch(), strftime() or timediff()? Walks back
** to the enclosing '(' (earlier literals are already blanked to '#') and
** reads the identifier before it. */
static int isTimeFunctionArg(const char *z, size_t i) {
  static const char *aTime[] = { "date", "time", "datetime", "julianday", "unixepoch", "strftime", "timediff", NULL };
  int depth = 0;
  size_t k = i, start, len;
  while (k > 0) {
    char c = z[--k];
    if (c == ')') depth++;
    else if (c == '(') { if (depth == 0) break; depth--; }
  }
  if (k == 0 && z[k] != '(') return 0;
  while (k > 0 && (z[k - 1] == ' ' || z[k - 1] == '\t' || z[k - 1] == '\n' || z[k - 1] == '\r')) k--;
  start = k;
  while (start > 0 && ((z[start - 1] >= 'a' && z[start - 1] <= 'z') || (z[start - 1] >= '0' && z[start - 1] <= '9') || z[start - 1] == '_')) start--;
  len = k - start;
  for (int f = 0; aTime[f]; f++) {
    if (strlen(aTime[f]) == len && strncmp(z + start, aTime[f], len) == 0) return 1;
  }
  return 0;
}

static int isNondeterministicSql(const char *zIn) {
  /* zIn is lowercased by the caller; build a copy with literals blanked */
  size_t n = strlen(zIn), i;
  char *z = sqlite3_malloc64(n + 1);
  int hit = 0;
  if (!z) return 0;
  memcpy(z, zIn, n + 1);
  for (i = 0; i < n; i++) {
    if (z[i] == '\'') {
      size_t j = i + 1;
      while (j < n && !(z[j] == '\'' && z[j + 1] != '\'')) j += (z[j] == '\'') ? 2 : 1;
      /* 'now' counts when the function it is an argument of is a time
      ** function: datetime('now'), strftime('%s', 'now'). VALUES('now'),
      ** x IN ('now') and upper('now') are data. */
      if (j - i - 1 == 3 && strncmp(z + i + 1, "now", 3) == 0 && isTimeFunctionArg(z, i)) hit = 1;
      for (size_t m = i; m <= j && m < n; m++) z[m] = '#';   /* a blanked literal: not an identifier char, not empty */
      i = j;
    } else if (z[i] == '"' || z[i] == '`') {
      /* identifiers: left as is (function names cannot hide in them) */
    }
  }
  if (hit) { sqlite3_free(z); return 1; }
  static const char *aFn[] = {
    "current_time", "current_date", "current_timestamp", NULL
  };
  /* date(), time(), datetime(), julianday(), unixepoch() with no argument,
  ** and strftime('fmt') with only a format, all mean the current time */
  static const char *aTime[] = { "date(", "time(", "datetime(", "julianday(", "unixepoch(", NULL };
  for (i = 0; aFn[i]; i++) {
    if (strstr(z, aFn[i])) { sqlite3_free(z); return 1; }
  }
  for (i = 0; aTime[i]; i++) {
    const char *p = z;
    size_t n = strlen(aTime[i]);
    while ((p = strstr(p, aTime[i])) != NULL) {
      const char *q = p + n;
      int boundary = (p == z) || !((p[-1] >= 'a' && p[-1] <= 'z') || p[-1] == '_');
      while (*q == ' ' || *q == '\t' || *q == '\n') q++;
      if (boundary && *q == ')') { sqlite3_free(z); return 1; }
      p += n;
    }
  }
  {
    /* strftime('fmt') with only a format: the literal is blanked above, so
    ** look for strftime( followed by blanks and ')' with nothing else */
    const char *p = strstr(z, "strftime(");
    if (p) {
      const char *q = p + 9;
      while (*q == ' ' || *q == '\t' || *q == '#') q++;     /* '#' = the blanked format literal */
      if (*q == ')') { sqlite3_free(z); return 1; }
    }
  }
  sqlite3_free(z);
  return 0;
}

static int isIdentChar(char c);
/*
** Functions in writes (#53, #54). A captured write is replayed on a
** connection that has SQLite's built-in functions and nothing the
** application registered (Django adds ~40 on every connection). Its
** functions are found by preparing its text again on the application
** connection with the authorizer collecting (prepare walks views, triggers,
** generated columns and indexes too); the verdict is cached by text and
** schema version. Reads are never checked: they are not logged.
*/
static int cmpFnName(const void *a, const void *b) {
  return strcmp(*(const char *const*)a, *(const char *const*)b);
}
static void loadReplicaFunctions(HppliteCtx *pCtx) {
  sqlite3 *m = NULL;
  sqlite3_stmt *st = NULL;
  int n = 0, cap = 0;
  char **a = NULL;
  pCtx->nReplicaFns = -1;
  if (sqlite3_open_v2(":memory:", &m, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK
      || sqlite3_prepare_v2(m, "SELECT DISTINCT lower(name) FROM pragma_function_list", -1, &st, NULL) != SQLITE_OK) {
    sqlite3_finalize(st);
    sqlite3_close(m);
    return;
  }
  while (sqlite3_step(st) == SQLITE_ROW) {
    const char *z = (const char*)sqlite3_column_text(st, 0);
    if (!z) continue;
    if (n == cap) {
      char **b = sqlite3_realloc64(a, sizeof(char*) * (cap = cap ? cap * 2 : 256));
      if (!b) break;
      a = b;
    }
    a[n++] = sqlite3_mprintf("%s", z);
  }
  sqlite3_finalize(st);
  sqlite3_close(m);
  if (n > 0) {
    qsort(a, n, sizeof(char*), cmpFnName);
    pCtx->azReplicaFns = a;
    pCtx->nReplicaFns = n;
  } else {
    sqlite3_free(a);
  }
}
void hppliteFreeFunctionChecks(HppliteCtx *pCtx) {
  int i;
  for (i = 0; i < pCtx->nReplicaFns; i++) sqlite3_free(pCtx->azReplicaFns[i]);
  sqlite3_free(pCtx->azReplicaFns);
  pCtx->azReplicaFns = NULL;
  pCtx->nReplicaFns = 0;
  for (i = 0; i < (int)(sizeof(pCtx->aFnVerdict) / sizeof(pCtx->aFnVerdict[0])); i++) {
    sqlite3_free(pCtx->aFnVerdict[i].zSql);
    pCtx->aFnVerdict[i].zSql = NULL;
  }
}
static void collectFunction(HppliteCtx *pCtx, const char *zName) {
  static const char *aNondet[] = { "random", "randomblob", "last_insert_rowid", "changes", "total_changes",
                                   "sqlite_version", "sqlite_source_id", "sqlite_compileoption_used",
                                   "sqlite_compileoption_get", "load_extension", NULL };
  char lower[64];
  const char *pLower = lower;
  size_t i, n = strlen(zName);
  if (n >= sizeof(lower)) n = sizeof(lower) - 1;
  for (i = 0; i < n; i++) lower[i] = (zName[i] >= 'A' && zName[i] <= 'Z') ? (char)(zName[i] + 32) : zName[i];
  lower[n] = 0;
  for (i = 0; aNondet[i]; i++) {
    if (strcmp(lower, aNondet[i]) == 0) {
      if (pCtx->zCollectWhy[0]) return;           /* the first reason stays the reason */
      snprintf(pCtx->zCollectWhy, sizeof(pCtx->zCollectWhy),
               "non-deterministic function %s() in a write; replay cannot reproduce it", lower);
      pCtx->collectKind = TEXT_VALUE;
      return;
    }
  }
  /* SQLite's internal functions (sqlite_rename_table ... for ALTER) are
  ** hidden from pragma_function_list and callable only by SQLite itself */
  if (strncmp(lower, "sqlite_", 7) == 0) return;
  if (pCtx->nReplicaFns == 0) loadReplicaFunctions(pCtx);
  if (pCtx->nReplicaFns > 0
      && !bsearch(&pLower, pCtx->azReplicaFns, pCtx->nReplicaFns, sizeof(char*), cmpFnName)) {
    snprintf(pCtx->zCollectWhy, sizeof(pCtx->zCollectWhy),
             "%s() is defined by the application, not SQLite: a replica does not have it, so a write "
             "that calls it cannot replay", lower);
    pCtx->collectKind = TEXT_FUNCTION;
  }
}
/* The statement's shape: string and number literals blanked. Which
** functions a statement calls cannot depend on a literal's value, so a loop
** of INSERTs with different values shares one verdict. Caller frees. */
static char *statementShape(const char *z) {
  char *out = sqlite3_malloc64(strlen(z) + 1), *o = out;
  char prev = ' ';
  if (!out) return NULL;
  while (*z) {
    if (*z == '\'') {                                  /* 'string' ('' escapes) */
      z++;
      while (*z && !(*z == '\'' && z[1] != '\'')) z += (*z == '\'') ? 2 : 1;
      if (*z) z++;
      *o++ = '?'; prev = '?';
    } else if (*z == '"' || *z == '`' || *z == '[') {   /* quoted identifier: kept */
      char q = (*z == '[') ? ']' : *z;
      *o++ = *z++;
      while (*z && *z != q) *o++ = *z++;
      if (*z) *o++ = *z++;
      prev = 'x';
    } else if (*z >= '0' && *z <= '9' && !isIdentChar(prev)) {
      while (isIdentChar(*z) || *z == '.') z++;         /* 12, 1.5, 0x1F, 1e5 */
      if ((z[-1] == 'e' || z[-1] == 'E') && (*z == '+' || *z == '-')) { z++; while (*z >= '0' && *z <= '9') z++; }
      *o++ = '#'; prev = '#';
    } else {
      prev = *o++ = *z++;
    }
  }
  *o = 0;
  return out;
}
/* The reason the write cannot replay by its functions, or NULL; *pKind says
** which: TEXT_NEVER when the check itself failed, TEXT_FUNCTION for a
** function the replica does not have, TEXT_VALUE for a non-deterministic one */
static const char *functionVerdict(HppliteCtx *pCtx, sqlite3_stmt *pS, TextVerdict *pKind) {
  const char *zSql = sqlite3_sql(pS);
  struct HppliteFnVerdict *v;
  sqlite3_stmt *p = NULL;
  int schema, rc;
  uint64_t h = 1469598103934665603ULL;
  const char *c;
  char *zShape;
  *pKind = TEXT_NEVER;
  if (!zSql) return NULL;
  zShape = statementShape(zSql);
  if (!zShape) {
    snprintf(pCtx->zCollectWhy, sizeof(pCtx->zCollectWhy), "could not check the functions of a write (out of memory)");
    return pCtx->zCollectWhy;
  }
  for (c = zShape; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ULL;
  /* The key must not read the file: a read of our own here, while the
  ** application statement is starting, races a bookkeeping commit (WAL
  ** snapshot, see hppliteBkBegin). The data version is in memory and moves when
  ** another connection commits; our own DDL is counted as it is captured.
  ** Left open: another connection adding a trigger that calls an
  ** application function, between our last lock and this statement. */
  {
    unsigned int dv = 0;
    sqlite3_file_control(pCtx->db, "main", SQLITE_FCNTL_DATA_VERSION, &dv);
    schema = (int)(dv * 2654435761u) ^ pCtx->nDdlGen;
  }
  v = &pCtx->aFnVerdict[h % (sizeof(pCtx->aFnVerdict) / sizeof(pCtx->aFnVerdict[0]))];
  if (v->zSql && v->schema == schema && strcmp(v->zSql, zShape) == 0) {
    sqlite3_free(zShape);
    *pKind = v->kind;
    return v->zWhy[0] ? v->zWhy : NULL;
  }
  pCtx->zCollectWhy[0] = 0;
  pCtx->collectKind = TEXT_REPLAYS;
  pCtx->bCollectFns = 1;
  rc = sqlite3_prepare_v2(pCtx->db, zSql, -1, &p, NULL);
  pCtx->bCollectFns = 0;
  sqlite3_finalize(p);
  if (rc != SQLITE_OK) {
    sqlite3_free(zShape);
    snprintf(pCtx->zCollectWhy, sizeof(pCtx->zCollectWhy), "could not check the functions of a write (%s)",
             sqlite3_errstr(rc));
    return pCtx->zCollectWhy;             /* not cached: may be transient */
  }
  sqlite3_free(v->zSql);
  v->zSql = zShape;
  v->schema = schema;
  v->kind = pCtx->collectKind;
  memcpy(v->zWhy, pCtx->zCollectWhy, sizeof(v->zWhy));
  *pKind = v->kind;
  return v->zWhy[0] ? v->zWhy : NULL;
}

/*
** Authorizer: the parser's view, so quoting, case and comments cannot hide
** anything. Application statements (bInternal clear) may not write our
** tables (the log and meta are not user data: #18), ATTACH another file
** (its writes are not captured); in warn mode ATTACH is only counted. Non-deterministic functions are caught
** here by name too, which the text scan could only approximate.
*/
static int isHppliteTable(const char *zName) {
  return zName && sqlite3_strnicmp(zName, "_hpplite_", 9) == 0;
}
/* Row capture's triggers (#19): only HPPLite makes or drops them */
static int isCaptureTrigger(const char *zName) {
  return zName && sqlite3_strnicmp(zName, "hpplite_cap_", 12) == 0;
}
static int hppliteAuthorizerInner(HppliteCtx *pCtx, int op, const char *z1, const char *z2) {
  if (op == SQLITE_DROP_TABLE || op == SQLITE_DROP_TEMP_TABLE) {
    snprintf(pCtx->zDropTable, sizeof(pCtx->zDropTable), "%s", z1 ? z1 : "");
    pCtx->nDropAllowed = 3;
  }
  if ((op == SQLITE_DROP_TRIGGER || op == SQLITE_DROP_TEMP_TRIGGER) && isCaptureTrigger(z1)
      && z2 && pCtx->zDropTable[0] && pCtx->nDropAllowed > 0 && sqlite3_stricmp(z2, pCtx->zDropTable) == 0) {
    /* a table's drop takes its triggers, ours too (rebuilt after): the three
    ** of that table, in the prepare of that DROP TABLE - a DROP TRIGGER
    ** prepared after it, the DROP TABLE never run, finds nothing left to
    ** allow (#71) */
    if (--pCtx->nDropAllowed == 0) pCtx->zDropTable[0] = 0;
    return SQLITE_OK;
  }
  if ((op == SQLITE_CREATE_TRIGGER || op == SQLITE_CREATE_TEMP_TRIGGER
       || op == SQLITE_DROP_TRIGGER || op == SQLITE_DROP_TEMP_TRIGGER)
      && isCaptureTrigger(z1)) {
    pCtx->zDenyWhy = "the hpplite_cap_ triggers record this connection's writes; only HPPLite changes them";
    pCtx->nAuthDenied++;
    return SQLITE_DENY;
  }
  switch (op) {
    case SQLITE_INSERT: case SQLITE_UPDATE: case SQLITE_DELETE:
    case SQLITE_DROP_TABLE: case SQLITE_DROP_INDEX: case SQLITE_DROP_TRIGGER: case SQLITE_DROP_VIEW:
    case SQLITE_DROP_TEMP_TABLE: case SQLITE_DROP_TEMP_INDEX: case SQLITE_DROP_TEMP_TRIGGER: case SQLITE_DROP_TEMP_VIEW:
    case SQLITE_ALTER_TABLE:
      if (isHppliteTable(z1) || (op == SQLITE_ALTER_TABLE && isHppliteTable(z2))) {
        pCtx->zDenyWhy = "HPPLite's own tables are not writable from the application";
        pCtx->nAuthDenied++;
        return SQLITE_DENY;
      }
      return SQLITE_OK;
    case SQLITE_CREATE_INDEX: case SQLITE_CREATE_TRIGGER:
    case SQLITE_CREATE_TEMP_INDEX: case SQLITE_CREATE_TEMP_TRIGGER:
      /* z1 = index/trigger name, z2 = its table: a trigger on the log would
      ** run (uncaptured) on every write we make */
      if (isHppliteTable(z1) || isHppliteTable(z2)) {
        pCtx->zDenyWhy = "HPPLite's own tables are not writable from the application";
        pCtx->nAuthDenied++;
        return SQLITE_DENY;
      }
      return SQLITE_OK;
    case SQLITE_CREATE_TABLE: case SQLITE_CREATE_VIEW:
    case SQLITE_CREATE_TEMP_TABLE: case SQLITE_CREATE_TEMP_VIEW:
      if (isHppliteTable(z1)) { pCtx->zDenyWhy = "the _hpplite_ prefix is reserved"; pCtx->nAuthDenied++; return SQLITE_DENY; }
      return SQLITE_OK;
    case SQLITE_ATTACH:
      pCtx->nNondeterministic++;
      if (pCtx->bDeterminismWarn) return SQLITE_OK;
      pCtx->zDenyWhy = "ATTACH: writes to another file are not captured";
      pCtx->nAuthDenied++;
      return SQLITE_DENY;
    case SQLITE_PRAGMA:
      /* foreign_keys / recursive_triggers are allowed and replayed in the
      ** mode each statement ran in (#52); foreign keys are read from the
      ** connection flags, recursive_triggers only after it was touched */
      if (z1 && sqlite3_stricmp(z1, "recursive_triggers") == 0) pCtx->bRecTrigSeen = 1;
      if (z1 && z2) pragmaModeSet(pCtx, z1, z2);
      /* The file stays in WAL: HPPLite's bookkeeping connection reads beside
      ** the application, and a journal that is off or truncated would void
      ** the crash safety the log relies on. Leaving WAL failed anyway, as
      ** "database is locked" (the bookkeeping connection holds it open):
      ** say why instead. Reading the mode is fine. */
      if (z1 && z2 && sqlite3_stricmp(z1, "journal_mode") == 0 && sqlite3_stricmp(z2, "wal") != 0
          && pCtx->bkDb != pCtx->db) {
        pCtx->zDenyWhy = "HPPLite keeps the database in WAL mode (journal_mode cannot be changed)";
        pCtx->nAuthDenied++;
        return SQLITE_DENY;
      }
      return SQLITE_OK;
    case SQLITE_FUNCTION:
      /* Checked when a write is captured (functionVerdict), not here: a
      ** read that calls random() never reaches the log (#54) */
      return SQLITE_OK;
    default:
      return SQLITE_OK;
  }
}
int hppliteAuthorizer(void *pArg, int op, const char *z1, const char *z2, const char *z3, const char *z4) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  int rc;
  (void)z3;
  if (!pCtx) return SQLITE_OK;
  if (pCtx->bCollectFns) {
    /* the capture triggers' functions are ours, and silent in a text statement */
    if (pCtx->bRows && z4 && sqlite3_strnicmp(z4, "hpplite_cap_", 12) == 0) return SQLITE_OK;
    /* every function, until one the replica lacks is found: that verdict
    ** wins over a non-deterministic builtin seen before it (#115) */
    if (op == SQLITE_FUNCTION && z2 && pCtx->collectKind != TEXT_FUNCTION) collectFunction(pCtx, z2);
    return SQLITE_OK;
  }
  if (pCtx->bInternal || !pCtx->bEnabled) return SQLITE_OK;
  if (pCtx->pRefusedStmt) {
    /* the reprepare of a refused statement whose first run failed stale:
    ** SQLite reruns it with the interrupt cleared and no trace, so it is
    ** stopped here, once (the next prepare on the connection is it) (#89).
    ** That reprepare runs inside the same sqlite3_step(), with the
    ** connection's error code still SQLITE_SCHEMA from the failed run; a
    ** prepare of the application's never is. The refused statement can be
    ** expired for other reasons too - its interrupt rolled back a
    ** transaction with a schema change in it, which expires every
    ** statement - and no rerun follows then (#101) */
    int bReprepare = sqlite3_errcode(pCtx->db) == SQLITE_SCHEMA || pCtx->bDenyReprepare;
    pCtx->pRefusedStmt = NULL;
    pCtx->bDenyReprepare = 0;
    if (bReprepare) return SQLITE_DENY;
  }
  /* VACUUM copies the database through one it attaches itself (its own
  ** statements, tables named as ours included); it is logged as text */
  if (pCtx->pVacuumStmt) return SQLITE_OK;
  /* the capture triggers' own writes into the log (z4: the trigger running) */
  if (pCtx->bRows && z4 && sqlite3_strnicmp(z4, "hpplite_cap_", 12) == 0
      && (op == SQLITE_INSERT || op == SQLITE_READ || op == SQLITE_FUNCTION || op == SQLITE_SELECT)) return SQLITE_OK;
  rc = hppliteAuthorizerInner(pCtx, op, z1, z2);
  if (rc == SQLITE_DENY) {
    fprintf(stderr, "hpplite: %s; statement refused (SQLITE_AUTH)\n", pCtx->zDenyWhy ? pCtx->zDenyWhy : "not authorized");
    snprintf(pCtx->zLastRefusal, sizeof(pCtx->zLastRefusal), "%s%s%s (the driver reports \"not authorized\")",
             pCtx->zDenyWhy ? pCtx->zDenyWhy : "not authorized", z1 ? ": " : "", z1 ? z1 : "");
  }
  return rc;
}

static char *lowercaseDup(const char *z) {
  size_t n = strlen(z), i;
  char *out = sqlite3_malloc64(n + 1);
  if (!out) return NULL;
  for (i = 0; i < n; i++) {
    char c = z[i];
    out[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
  }
  out[n] = 0;
  return out;
}

/*
** Does the statement reference one of our own tables? String literals do
** not count: 'see _hpplite_pending' is user data and must be captured.
*/
static int isIdentChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int mentionsHppliteTable(const char *z) {
  char prev = ' ';
  while (*z) {
    if (*z == '\'' || *z == '"' || *z == '`') {
      char q = *z++;
      while (*z && !(*z == q && z[1] != q)) z += (*z == q) ? 2 : 1;
      if (*z) z++;
      prev = 'x';
    } else if (*z == '[') {                          /* [bracketed identifier] */
      while (*z && *z != ']') z++;
      if (*z) z++;
      prev = 'x';
    } else if (z[0] == '-' && z[1] == '-') {           /* -- comment to end of line */
      while (*z && *z != '\n') z++;
    } else if (z[0] == '/' && z[1] == '*') {           /* block comment */
      const char *e = strstr(z + 2, "*/");
      z = e ? e + 2 : z + strlen(z);
    } else if (strncmp(z, "_hpplite_", 9) == 0 && !isIdentChar(prev)) {
      return 1;                                        /* an identifier that starts with our prefix */
    } else {
      prev = *z++;
    }
  }
  return 0;
}

/*
** Capture failed: the statement must not run either, or the log and the
** database disagree forever. sqlite3_interrupt() makes the statement that
** is about to execute fail with SQLITE_INTERRUPT (inside an explicit
** transaction SQLite rolls the transaction back, as a disk-full would).
*/
static void refuseStatement(HppliteCtx *pCtx, sqlite3_stmt *pS, const char *zWhy, const char *zSql) {
  pCtx->nLogErrors++;
  pCtx->pRefusedStmt = pS;
  fprintf(stderr, "hpplite: %s; statement refused (SQLITE_INTERRUPT): %.80s\n", zWhy, zSql);
  snprintf(pCtx->zLastRefusal, sizeof(pCtx->zLastRefusal), "%s (the driver reports \"interrupted\"): %.160s", zWhy, zSql);
  sqlite3_interrupt(pCtx->db);
}

/* 'localtime' / 'utc' as a time function's modifier; zIn lower-cased.
** Literals already looked at are blanked, as isTimeFunctionArg expects. */
static int usesTimeZone(const char *zIn) {
  size_t n = strlen(zIn), i;
  char *z = sqlite3_malloc64(n + 1);
  int hit = 0;
  if (!z) return 0;
  memcpy(z, zIn, n + 1);
  for (i = 0; i < n && !hit; i++) {
    if (z[i] == '\'') {
      size_t j = i + 1;
      while (j < n && !(z[j] == '\'' && z[j + 1] != '\'')) j += (z[j] == '\'') ? 2 : 1;
      if (((j - i - 1 == 9 && strncmp(z + i + 1, "localtime", 9) == 0)
           || (j - i - 1 == 3 && strncmp(z + i + 1, "utc", 3) == 0)) && isTimeFunctionArg(z, i)) {
        hit = 1;
      }
      for (size_t m = i; m <= j && m < n; m++) z[m] = '#';
      i = j;
    }
  }
  sqlite3_free(z);
  return hit;
}

/*
** Why a statement logged as text could not replay (#104): the one place the
** text rules are applied, in order, with no effect on the connection (the
** function check keeps its cache; see functionVerdict). Writes the reason,
** with its advice, into zWhy. A value replay cannot reproduce (TEXT_VALUE)
** is let through when bWarn (determinism=warn), and the later rules still
** apply; a function the replica lacks or a statement that cannot be logged
** never is. *pnHits counts the determinism rules hit, let through or not,
** for hpplite_status.nondeterministic. Only for a tracked statement
** (isTrackedSql), as the rules presume one. Returns TEXT_REPLAYS when the
** statement may be logged (zWhy then holds the last let-through reason, or
** nothing).
*/
static TextVerdict textVerdict(HppliteCtx *pCtx, sqlite3_stmt *pS, const char *zSql, int bWarn,
                               char *zWhy, size_t nWhy, int *pnHits) {
  /* the refusal says why this statement is text, so the rule is not taken
  ** for row capture's (#85) */
  const char *zAsText = pCtx->bRows
    ? " - this statement is logged as text (a schema change, a write reaching a virtual table, or a table another"
      " connection just created) and replays as written"
    : "";
  const char *zWhat = NULL;
  TextVerdict kind;
  char *lower = lowercaseDup(zSql);
  *pnHits = 0;
  zWhy[0] = 0;
  if (lower) {
    /* the clock replays when the connection is on the clock VFS (#56);
    ** a machine's time zone never does */
    if (usesTimeZone(lower))
      zWhat = "'localtime'/'utc' depend on the machine's time zone, which differs between replicas";
    else if (!pCtx->bClockPin && isNondeterministicSql(lower))
      zWhat = "non-deterministic SQL (random(), 'now', CURRENT_TIMESTAMP, date() ...) cannot be replayed";
    sqlite3_free(lower);
  }
  if (zWhat) {
    (*pnHits)++;
    snprintf(zWhy, nWhy, "%s%s; bind the value instead", zWhat, zAsText);
    if (!bWarn) return TEXT_VALUE;
  }
  if ((pCtx->pragmaMode & HPPLITE_MODE_WRITABLE_SCHEMA) && hpplite_sql_whole_schema_write(zSql)) {
    /* every schema row, HPPLite's too: "malformed database schema" on the
    ** next open (#124); shadow tables (#18) are the structural answer */
    snprintf(zWhy, nWhy, "an UPDATE or DELETE on sqlite_schema with no WHERE rewrites HPPLite's own schema rows as well;"
                         " name the table (WHERE name = '...')");
    return TEXT_NEVER;
  }
  if (mentionsHppliteTable(zSql)) {
    /* A write that names our tables where the authorizer could not see it
    ** (RENAME TO _hpplite_x, a subquery over the log whose rows differ on
    ** every replica): not logged, so not allowed either. */
    snprintf(zWhy, nWhy, "application writes may not reference _hpplite_ tables");
    return TEXT_NEVER;
  }
  zWhat = functionVerdict(pCtx, pS, &kind);
  if (zWhat && kind == TEXT_NEVER) {
    /* the check itself failed (out of memory): nothing to advise */
    snprintf(zWhy, nWhy, "%s", zWhat);
    return TEXT_NEVER;
  }
  if (zWhat) {
    (*pnHits)++;
    snprintf(zWhy, nWhy, "%s%s; compute the value in the application and bind it", zWhat, zAsText);
    if (kind == TEXT_FUNCTION || !bWarn) return kind;
  }
  /* A SQLite that cuts bound values in the expansion is used with row
  ** capture only (#62): the few statements still logged as text are
  ** refused when they bind anything, which would replay cut */
  if (pCtx->bExpandTruncates && pS && sqlite3_bind_parameter_count(pS) > 0) {
    snprintf(zWhy, nWhy, "this SQLite truncates bound values in sqlite3_expanded_sql() (better-sqlite3's "
             "bundled SQLite), and this statement is logged as text (a schema change, a write that "
             "reaches a virtual table, or the first write to a table another connection just "
             "created): put the values in its text, or run it on a stock SQLite");
    return TEXT_NEVER;
  }
  return TEXT_REPLAYS;
}

/*
** Trace callback. SQLITE_TRACE_STMT runs at the start of every statement on
** the application connection: the statement text is written into
** _hpplite_pending through the same connection, so the row is part of
** whatever transaction the statement runs in - it commits with the
** statement and rolls back with it. SQLITE_TRACE_PROFILE runs when the
** statement finishes and removes the row again if the statement failed.
*/
/*
** The connection state a statement runs under that its text does not show
** (#52). Foreign keys come from the connection's flags (no statement);
** recursive_triggers has no such accessor, so it is read with the pragma -
** only once the application has prepared that pragma at all.
** Called with bInternal set (the reads are not captured or authorized).
*/
static int currentMode(HppliteCtx *pCtx) {
  int mode = 0, fk = 0;
  if (sqlite3_db_config(pCtx->db, SQLITE_DBCONFIG_ENABLE_FKEY, -1, &fk) == SQLITE_OK && fk) {
    mode |= HPPLITE_MODE_FOREIGN_KEYS;
  }
  if (pCtx->bRecTrigSeen) {
    if (!pCtx->pRecTrigStmt
        && sqlite3_prepare_v3(pCtx->db, "PRAGMA recursive_triggers", -1, SQLITE_PREPARE_PERSISTENT,
                              &pCtx->pRecTrigStmt, NULL) != SQLITE_OK) {
      pCtx->pRecTrigStmt = NULL;
    }
    if (pCtx->pRecTrigStmt) {
      if (sqlite3_step(pCtx->pRecTrigStmt) == SQLITE_ROW && sqlite3_column_int(pCtx->pRecTrigStmt, 0)) {
        mode |= HPPLITE_MODE_RECURSIVE_TRIGGERS;
      }
      sqlite3_reset(pCtx->pRecTrigStmt);
    }
  }
  return mode | pCtx->pragmaMode;
}

/*
** The transaction gate. In WAL mode a bookkeeping commit that lands after an
** application transaction has read makes that transaction's next write fail
** ("database is locked": its snapshot can no longer become a write). Checks
** at the start of the bookkeeping transaction leave a window; the gate does
** not: the application holds it from the start of a statement until no
** statement runs and no transaction is open, and a bookkeeping write takes
** it or does not happen (the batch is adopted by a later flush). A flag, not
** a mutex: several application threads may share the connection.
*/
static void gateEnterApp(HppliteCtx *pCtx) {
  int expected;
  if (pCtx->bkDb == pCtx->db) return;
  for (;;) {
    if (__atomic_load_n(&pCtx->txnGate, __ATOMIC_ACQUIRE) == 1) return;
    expected = 0;
    if (__atomic_compare_exchange_n(&pCtx->txnGate, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    usleep(200);                          /* a bookkeeping write: milliseconds */
  }
}
static void gateLeaveAppIfIdle(HppliteCtx *pCtx) {
  if (pCtx->bkDb == pCtx->db) return;
  if (pCtx->nAppStmtActive == 0 && sqlite3_get_autocommit(pCtx->db)
      && __atomic_load_n(&pCtx->txnGate, __ATOMIC_ACQUIRE) == 1) {
    __atomic_store_n(&pCtx->txnGate, 0, __ATOMIC_RELEASE);
  }
}

/* sqlite_sequence changes without changes() saying so (a zero-row INSERT
** into an AUTOINCREMENT table creates its entry): a fingerprint of it */
static int seqFingerprint(HppliteCtx *pCtx, sqlite3_int64 out[2]) {
  if (!pCtx->pSeqStmt) {
    if (pCtx->seqTriedGen == pCtx->nDdlGen + 1) return 0;
    if (sqlite3_prepare_v3(pCtx->db, "SELECT count(*), total(seq) FROM sqlite_sequence", -1,
                           SQLITE_PREPARE_PERSISTENT, &pCtx->pSeqStmt, NULL) != SQLITE_OK) {
      pCtx->pSeqStmt = NULL;
      pCtx->seqTriedGen = pCtx->nDdlGen + 1;
      return 0;
    }
  }
  out[0] = out[1] = -1;
  if (sqlite3_step(pCtx->pSeqStmt) == SQLITE_ROW) {
    out[0] = sqlite3_column_int64(pCtx->pSeqStmt, 0);
    out[1] = (sqlite3_int64)sqlite3_column_double(pCtx->pSeqStmt, 1);
  }
  sqlite3_reset(pCtx->pSeqStmt);
  return 1;
}

/* The run of pS, if one is being tracked: its index in aRun, else -1 */
static int runFind(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  int i;
  for (i = 0; i < pCtx->nRun; i++) if (pCtx->aRun[i].pS == pS) return i;
  return -1;
}

/* A run of pS starts (its STMT trace, or the rerun of a stale statement
** that has no trace): tracked while there is room, the application counted
** as busy and holding the gate either way. Returns the index, or -1. */
static int runStart(HppliteCtx *pCtx, sqlite3_stmt *pS, sqlite3_int64 logId, unsigned clockSeen) {
  int slot = -1;
  if (pCtx->nRun < HPPLITE_MAX_RUNS) {
    struct HppliteStmtRun *r = &pCtx->aRun[pCtx->nRun];
    slot = pCtx->nRun++;
    r->pS = pS;
    r->logId = logId;
    r->now = 0;
    r->clockSeen = clockSeen;
    r->vmSteps = sqlite3_stmt_status(pS, SQLITE_STMTSTATUS_VM_STEP, 0);
  }
  gateEnterApp(pCtx);
  pCtx->nAppStmtActive++;
  return slot;
}

/* A run of an application statement is over: forget it (PROFILE, or found
** reset without finishing). Returns what was recorded for it. */
typedef struct ActiveRun { sqlite3_int64 log, now; unsigned reads; int found; } ActiveRun;
static ActiveRun endActiveRun(HppliteCtx *pCtx, int i) {
  ActiveRun r;
  int last = --pCtx->nRun;
  r.log = pCtx->aRun[i].logId; r.now = pCtx->aRun[i].now; r.found = 1;
  /* clock reads during the run, counted before the pin goes (it resets the
  ** count) and before our own statements below read it */
  r.reads = hpplite_clock_consulted() - pCtx->aRun[i].clockSeen;
  if (pCtx->aRun[i].pS == pCtx->pPinOwner) {
    hpplite_clock_pin(0);
    pCtx->pPinOwner = NULL;
  }
  pCtx->aRun[i] = pCtx->aRun[last];
  if (pCtx->nAppStmtActive > 0) pCtx->nAppStmtActive--;
  return r;
}

/* Runs whose end went unreported (finalized while tracing was replaced,
** say) must not count as running forever - bookkeeping would never commit:
** runs that are no longer busy, or no longer exist, are over. Application
** thread. */
static void endResetRuns(HppliteCtx *pCtx, sqlite3_stmt *pExcept) {
  int j;
  for (j = pCtx->nRun - 1; j >= 0; j--) {
    sqlite3_stmt *p = NULL, *q = pCtx->aRun[j].pS;
    if (q == pExcept) continue;
    while ((p = sqlite3_next_stmt(pCtx->db, p)) != NULL) if (p == q) break;
    if (!p || !sqlite3_stmt_busy(q)) endActiveRun(pCtx, j);
  }
}

static void noteNoClock(HppliteCtx *pCtx, sqlite3_int64 id, sqlite3_int64 now) {
  if (!pCtx->pNoClockMutex) pCtx->pNoClockMutex = sqlite3_mutex_alloc(SQLITE_MUTEX_FAST);
  if (!pCtx->pNoClockMutex) return;
  sqlite3_mutex_enter(pCtx->pNoClockMutex);
  if (pCtx->nNoClock == pCtx->nNoClockAlloc) {
    int n = pCtx->nNoClockAlloc ? pCtx->nNoClockAlloc * 2 : 256;
    struct HppliteNoClock *a = sqlite3_realloc64(pCtx->aNoClock, sizeof(*a) * (size_t)n);
    if (a) { pCtx->aNoClock = a; pCtx->nNoClockAlloc = n; }
  }
  if (pCtx->nNoClock < pCtx->nNoClockAlloc) {
    pCtx->aNoClock[pCtx->nNoClock].id = id;
    pCtx->aNoClock[pCtx->nNoClock].now = now;
    pCtx->nNoClock++;
  }
  sqlite3_mutex_leave(pCtx->pNoClockMutex);
}

/*
** A statement prepared on a stale schema - another connection changed it -
** fails its first run with SQLITE_SCHEMA and SQLite reruns it inside
** sqlite3_step(), without a STMT trace (tag-20220401a): in autocommit the
** failed run rolls back our log row, and the rerun would go unlogged. So
** once our row is written (in the write transaction: a read before it would
** hold a snapshot the write could not upgrade from), HPPLite reads the schema
** version; if it moved since this connection last saw it, the statement is
** stale, and HPPLite turns the transaction into one of its own (BEGIN, before
** the statement's own transaction start). The failed run then undoes only
** itself; our row stays; the rerun runs in the same transaction, which
** HPPLite commits at its end.
*/
/* Track pS as a statement SQLite may rerun stale: in autocommit inside a
** transaction of HPPLite's (the failed first run would take our row with
** it), in the application's transaction as it is (only the failed run's own
** statement rolls back there - but its PROFILE must not unlog the row) */
static int rerunTrack(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  if (pCtx->pRerunStmt) return 0;
  if (getenv("HPPLITE_TRACE_RERUN"))
    fprintf(stderr, "hpplite: rerun-tracking (schema %d, seen %d, autocommit %d): %.80s\n", hppliteSchemaVersion(pCtx),
            pCtx->seenSchema, sqlite3_get_autocommit(pCtx->db), sqlite3_sql(pS) ? sqlite3_sql(pS) : "?");
  if (sqlite3_get_autocommit(pCtx->db)) {
    if (hppliteInternalExec(pCtx, pCtx->db, "BEGIN") != SQLITE_OK) return 0;
    pCtx->ownTxn = HPPLITE_OWN_OPEN;
  } else if (pCtx->ownTxn != HPPLITE_OWN_COMMIT_PENDING) {
    /* the application's transaction, left alone - or ours, its commit still
    ** owed behind a statement in progress: this statement runs inside it and
    ** the commit stays due at the next end */
    pCtx->ownTxn = HPPLITE_OWN_NONE;
  }
  pCtx->pRerunStmt = pS;
  pCtx->rerunStage = 1;
  pCtx->rerunSchema = hppliteSchemaVersion(pCtx);
  return 1;
}
static void rerunBegin(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  int v;
  if (pCtx->pRerunStmt) return;
  v = hppliteSchemaVersion(pCtx);
  if (v >= 0 && pCtx->seenSchema >= 0 && v != pCtx->seenSchema) rerunTrack(pCtx, pS);
  pCtx->seenSchema = v;
}
/* HPPLite's own transaction ends: COMMIT, and what the commit hook does for
** an application commit (ours it skips). The one place it is committed.
** Returns 1 when it is over (committed, or already ended by the application),
** 0 when the COMMIT failed - a statement in progress (this is its trace):
** ours stays open with the application's statement inside, and is committed
** at that statement's end - never left open past it. */
static int ownTxnEnd(HppliteCtx *pCtx) {
  if (pCtx->ownTxn == HPPLITE_OWN_NONE) return 1;
  if (sqlite3_get_autocommit(pCtx->db)) { pCtx->ownTxn = HPPLITE_OWN_NONE; return 1; }   /* ended by the application */
  if (hppliteInternalExec(pCtx, pCtx->db, "COMMIT") != SQLITE_OK) return 0;
  pCtx->ownTxn = HPPLITE_OWN_NONE;
  pCtx->curTxn = 0;
  pCtx->bRootDirty = 1;
  if (pCtx->xOnCommit) pCtx->xOnCommit(pCtx->pCallbackArg, NULL, 0);
  pCtx->bAppTxnOpen = 0;
  gateLeaveAppIfIdle(pCtx);
  return 1;
}
/* The tracked statement is over: forget it, end our transaction if it is ours */
static void rerunCommit(HppliteCtx *pCtx) {
  pCtx->pRerunStmt = NULL;
  pCtx->rerunStage = 0;
  pCtx->bCaptureDeferred = 0;
  if (!ownTxnEnd(pCtx)) pCtx->ownTxn = HPPLITE_OWN_COMMIT_PENDING;
  pCtx->bAppTxnOpen = !sqlite3_get_autocommit(pCtx->db);
  gateLeaveAppIfIdle(pCtx);
}
/* ...that end: the application's statement over, nothing is in progress */
static void rerunCommitRetry(HppliteCtx *pCtx) {
  if (!ownTxnEnd(pCtx)) {
    hpplite_error(pCtx, "could not commit the transaction opened for a stale statement's rerun (%s)",
            sqlite3_errmsg(pCtx->db));
    pCtx->nLogErrors++;
  }
}
/* Expired (to be prepared again)? A SQLite built without its deprecated
** interfaces has no sqlite3_expired(): assume so (the rerun's end, or the
** next statement's start, commits) */
static int stmtExpired(sqlite3_stmt *pS) {
#ifdef HPPLITE_EXTENSION
  if (!sqlite3_api->expired) return 1;
#endif
  return sqlite3_expired(pS);
}
static int captureStart(HppliteCtx *pCtx, sqlite3_stmt *pS, const char *zSql, int slot);
static int hppliteSchemaProbe(HppliteCtx *pCtx);
/* PROFILE of that statement: its first run failed and expired it - the rerun
** follows (running again, as a STMT trace would have recorded it); or it is
** over (it ran, or failed for another reason): commit */
static void rerunStatementDone(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  /* expired by a schema change of its own (a CREATE VIRTUAL TABLE's nested
  ** DDL expires every statement, this one too): it ran, no rerun follows */
  /* (the schema version is unknown when the capture was deferred - the read
  ** failed with the rest - and expiry alone says the rerun follows) */
  if (pCtx->rerunStage == 1 && stmtExpired(pS)
      && (pCtx->rerunSchema < 0 || hppliteSchemaVersion(pCtx) == pCtx->rerunSchema)) {
    int slot;
    /* the run still pins the stale schema (expired by a schema change of
    ** HPPLite's own, it failed before its cookie check): not yet - its next
    ** run resets the schema and comes back here, or fails and ends it */
    if (pCtx->bCaptureDeferred && hppliteSchemaProbe(pCtx) == SQLITE_SCHEMA) return;
    pCtx->rerunStage = 2;
    slot = runStart(pCtx, pS, pCtx->logged.id, hpplite_clock_consulted());
    if (pCtx->bCaptureDeferred) {
      /* now the schema is reset and HPPLite's statements run (#108): the
      ** capture the start could not do, before the rerun. Still deferred
      ** while it runs, so a second failure refuses instead of deferring */
      captureStart(pCtx, pS, sqlite3_sql(pS), slot);
      pCtx->bCaptureDeferred = 0;
    } else {
      pCtx->logged.pS = pS;             /* the effect check applies to the rerun */
    }
    return;
  }
  /* (a deferred capture whose statement did not expire: it never reached its
  ** schema - failed earlier, SQLITE_BUSY say - a statement on a stale schema
  ** that runs expires; nothing to capture, nothing ran) */
  pCtx->bCaptureDeferred = 0;
  rerunCommit(pCtx);
  pCtx->seenSchema = hppliteSchemaVersion(pCtx);
}

/* A VACUUM that ran, into the log as a group of its own; the application's
** last_insert_rowid() is kept (VACUUM is not a statement whose changes()
** anyone reads) */
static void logVacuum(HppliteCtx *pCtx) {
  sqlite3_stmt *q = NULL;
  sqlite3_int64 appRowid = sqlite3_last_insert_rowid(pCtx->db);
  int saved = hppliteInternalBegin(pCtx);
  if (sqlite3_prepare_v2(pCtx->db, "INSERT INTO _hpplite_pending(txn, sql, mode, now) VALUES(0, 'VACUUM', 0, 0)",
                         -1, &q, NULL) == SQLITE_OK) {
    if (hpplite_step_write(q) != SQLITE_DONE) pCtx->nLogErrors++;
  } else {
    pCtx->nLogErrors++;
  }
  sqlite3_finalize(q);
  sqlite3_set_last_insert_rowid(pCtx->db, appRowid);
  hppliteInternalEnd(pCtx, saved);
}

/* The text rules on a tracked statement about to be logged as text: counted,
** warned about, or refused (1). */
static int textRefused(HppliteCtx *pCtx, sqlite3_stmt *pS, const char *zSql) {
  char why[512];
  int nHits;
  TextVerdict v = textVerdict(pCtx, pS, zSql, pCtx->bDeterminismWarn, why, sizeof why, &nHits);
  pCtx->nNondeterministic += (uint64_t)nHits;
  if (v != TEXT_REPLAYS) {
    refuseStatement(pCtx, pS, why, zSql);
    return 1;
  }
  if (nHits && pCtx->nNondeterministic == (uint64_t)nHits) {   /* the first let through: said once */
    fprintf(stderr, "hpplite: %s; verification will report a mismatch: %.80s\n", why, zSql);
  }
  return 0;
}

/*
** HPPLite's own statement could not run at the application statement's
** start: the connection's schema is stale (another connection changed it)
** and SQLite does not reset it while a statement runs on the connection
** (3.55: the reset waits for every running statement, #108) - so our
** statement fails SQLITE_SCHEMA however often it is reprepared. The
** application's statement is stale too: its first run fails SQLITE_SCHEMA,
** SQLite resets the schema (no other statement runs then) and reruns it
** inside the same step, without a trace (rerunTrack). The capture is done
** at that failed run's end instead (rerunStatementDone), when our
** statements run again, in a transaction of HPPLite's opened here so the
** rerun joins it and the row commits with it. 1 when deferred; 0 when it
** cannot be: another statement is tracked already, or this is the deferred
** capture itself failing again (another statement of the application in
** progress pins the schema: the rerun will fail the same way).
*/
static int deferCapture(HppliteCtx *pCtx, sqlite3_stmt *pS, int bJudged) {
  if (pCtx->bCaptureDeferred) return 0;
  if (pCtx->pRerunStmt != pS && !rerunTrack(pCtx, pS)) return 0;   /* (tracked already: the #92 path) */
  pCtx->bCaptureDeferred = 1;
  pCtx->bDeferredJudged = bJudged;
  return 1;
}

/* Can HPPLite's statements run on the connection now? A read of its own
** table, cookie check included: SQLITE_SCHEMA when the connection's schema
** is stale and SQLite will not reset it (a running statement pins it, #108);
** on a SQLite that resets it under a running statement the read reprepares
** itself and the reset is done by it. Any other failure is returned as is. */
static int hppliteSchemaProbe(HppliteCtx *pCtx) {
  int rc, saved = hppliteInternalBegin(pCtx);
  if (!pCtx->pProbeStmt
      && sqlite3_prepare_v3(pCtx->db, "SELECT count(*) FROM main._hpplite_meta", -1, SQLITE_PREPARE_PERSISTENT,
                            &pCtx->pProbeStmt, NULL) != SQLITE_OK) {
    pCtx->pProbeStmt = NULL;
  }
  rc = pCtx->pProbeStmt ? sqlite3_step(pCtx->pProbeStmt) : SQLITE_ERROR;
  if (pCtx->pProbeStmt) sqlite3_reset(pCtx->pProbeStmt);
  hppliteInternalEnd(pCtx, saved);
  rc &= 0xff;
  return rc == SQLITE_ROW || rc == SQLITE_DONE ? SQLITE_OK : rc;
}

/*
** The capture of a write at its start (its STMT trace; or, deferred, the
** end of its failed first run, #108): as rows by the triggers when they
** cover it, else as text - the rules applied, the row written into
** _hpplite_pending in the write's own transaction. slot: its run in aRun,
** or -1.
*/
static int captureStart(HppliteCtx *pCtx, sqlite3_stmt *pS, const char *zSql, int slot) {
  char *zExpanded;
  int rc, saved, bDeferred = pCtx->bCaptureDeferred;
  /* Row capture (#19): the triggers record what a write changes - its text,
  ** its functions and the clock no longer matter. A write on a table they do
  ** not cover yet (created by another connection since the last rebuild) is
  ** logged as text, the v1 way, its triggers silent; rebuilt after it. So
  ** is a write that may reach a virtual table (FTS5, R*Tree): what those
  ** store cannot carry triggers. */
  if (pCtx->bRows) {
    int covered;
    /* a deferred capture (#108): the schema is reset now and the triggers
    ** can be rebuilt for it before the rerun is prepared - a write then
    ** records as rows, as it would have on a connection that saw the change
    ** at a statement's end. Not for DDL (a DROP COLUMN is rid of its triggers) */
    if (bDeferred && isRowsDml(zSql) && !hpplite_rows_covered(pCtx)) hpplite_rows_after_statement(pCtx);
    covered = isRowsDml(zSql) ? hpplite_rows_covered_for_write(pCtx) : 0;
    if (covered == HPPLITE_ROWS_STALE) {
      /* the lock no-op could not run: the connection's schema is stale and
      ** stays so while this statement runs (#108). It will be text (its
      ** triggers are stale): the rules now, so a refusal opens nothing;
      ** then captured at its first run's end, or refused */
      if (textRefused(pCtx, pS, zSql)) return 0;
      if (deferCapture(pCtx, pS, 1)) return 0;
      refuseStatement(pCtx, pS, "could not log statement (database schema has changed)", zSql);
      return 0;
    }
    if (covered == 0 && pCtx->bRebuildOwed && isRowsDml(zSql) && !hpplite_rows_text_only(pCtx, zSql)
        && rerunTrack(pCtx, pS)) {
      /* the rebuild owed since a write found the schema stale and pinned
      ** under a held lock and got SQLITE_BUSY (#108): its end could not do it
      ** (the statement still active there). Now, under this write's lock and
      ** in a transaction of HPPLite's. This statement was compiled with the
      ** old triggers and must not run as it is: the trigger DDL expires it,
      ** and a running statement so expired aborts at its first cursor
      ** (SQLITE_ABORT_ROLLBACK) rather than reruns - unless its own schema
      ** check fails first, which a schema cookie moved on past its own
      ** guarantees: SQLite then reprepares it, with the new triggers, inside
      ** the same step, as for any stale statement. The retry records as rows */
      int v = hppliteSchemaVersion(pCtx);
      hpplite_rows_after_statement(pCtx);
      if (v >= 0 && hpplite_rows_covered(pCtx)) {
        /* the cookie moves after the rebuild: moving it marks this
        ** connection's own schema for a reload too, which this statement's
        ** check does (no statement of ours could, with it running) */
        char *zBump = sqlite3_mprintf("PRAGMA main.schema_version = %d", v + 1);
        if (zBump && hppliteInternalExec(pCtx, pCtx->db, zBump) == SQLITE_OK) {
          pCtx->rerunSchema = v + 1;
          pCtx->rowsBuiltFor = v + 1;                  /* the same triggers, for the same tables */
          pCtx->logged.changes = -1;
          pCtx->logged.bSeq = 0;
          covered = 1;
        }
        sqlite3_free(zBump);
      }
    }
    if (covered == HPPLITE_ROWS_BUSY && !hpplite_rows_text_only(pCtx, zSql) && rerunTrack(pCtx, pS)) {
      /* The write lock is held elsewhere past busy_timeout (#92). Waiting
      ** again (the text path's own insert) and refusing would double the
      ** wait and end in SQLITE_INTERRUPT, which no driver retries. Instead
      ** the statement itself takes the lock, as a plain SQLite write would,
      ** under a snapshot opened here (rerunTrack's schema read, in a
      ** transaction of HPPLite's when in autocommit): with a read open
      ** SQLite does not wait again - the lock still held, or a commit since
      ** the snapshot, is SQLITE_BUSY at once, the driver's retry catches it
      ** and comes back through this trace; else the write runs on exactly
      ** the schema read here, and its triggers (rebuilt under the snapshot
      ** when stale) record it as rows. HPPLite's transaction is committed at
      ** the statement's end, as after a stale rerun. */
      if (hppliteSchemaProbe(pCtx) == SQLITE_SCHEMA && deferCapture(pCtx, pS, 0)) {
        pCtx->bRebuildOwed = 1;
        /* the schema is stale and pinned while this statement runs (#108):
        ** a rebuild now would drop the triggers and build none. The
        ** statement takes the lock itself: SQLITE_BUSY (nothing to capture;
        ** the triggers are rebuilt at its end, the retry is rows), or it
        ** runs stale and is captured at its failed first run's end, the
        ** triggers rebuilt there, the rules applied there if it is text */
        return 0;
      }
      hpplite_rows_after_statement(pCtx);            /* the rebuild, when the triggers are not the snapshot's */
      if (hpplite_rows_covered(pCtx)) {
        pCtx->logged.changes = -1;                     /* no text row: the rerun's effect check keeps nothing */
        pCtx->logged.bSeq = 0;
        covered = 1;
      }
    }
    if (covered == 1 && !hpplite_rows_text_only(pCtx, zSql)) {
      /* A text statement still open (a RETURNING not read to the end) did
      ** all its writing in its first step: its silence must not cover this
      ** one (#63). Its own next run arrives as a new trace, silenced again */
      pCtx->pRowsTextStmt = NULL;
      pCtx->bRowsCapOff = 0;
      return 0;
    }
    /* Everything else is text, and a statement's text replays all it does:
    ** its triggers record nothing - a DROP TABLE's implicit delete cascades
    ** through foreign keys too (Alembic rebuilding a self-referencing table
    ** with them on), and replaying the DROP cascades the same way */
    pCtx->pRowsTextStmt = pS;
    pCtx->bRowsCapOff = 1;
  }
  if (!isTrackedSql(zSql)) return 0;
  {
    const char *z = zSql;
    while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
    if (sqlite3_strnicmp(z, "CREATE", 6) == 0 || sqlite3_strnicmp(z, "DROP", 4) == 0
        || sqlite3_strnicmp(z, "ALTER", 5) == 0) {
      pCtx->nDdlGen++;                    /* views and triggers change what a write calls */
    }
  }
  /* Statements replay cannot reproduce are refused when logged as text
  ** (every write under capture=text; with row capture a schema change, a
  ** write reaching a virtual table, or a table another connection just
  ** created, #19). Refused by default so the application learns at the
  ** write, not from a root mismatch 100 batches later; determinism=warn
  ** keeps the old warn-and-count behaviour for a value (never for a
  ** function the replica lacks, or a statement that cannot be logged). */
  if ((!bDeferred || !pCtx->bDeferredJudged) && textRefused(pCtx, pS, zSql)) return 0;   /* (unless judged at its start) */

  /* Bound parameters are substituted so the statement can be replayed */
  zExpanded = sqlite3_expanded_sql(pS);
  if (!zExpanded) {
    refuseStatement(pCtx, pS, "cannot expand bound parameters (out of memory?)", zSql);
    return 0;
  }
  /* No size limit here: a transaction larger than a batch spans several
  ** (#57), and so does a statement - its text is posted in pieces (#59) */

  if (pCtx->bClockPin && slot < 0) {
    slot = runFind(pCtx, pS);
  }
  if (pCtx->bClockPin && slot >= 0) {
    /* Pin "now" for the statement (#56): the outermost write pins the real
    ** time, a write that starts while it runs shares it; at the end, a
    ** statement that read the clock records the time it read */
    sqlite3_int64 t = hpplite_clock_pinned();
    if (!t) {
      t = hpplite_clock_read();
      hpplite_clock_pin(t);
      pCtx->pPinOwner = pS;
    }
    pCtx->aRun[slot].now = t;
  }

  saved = hppliteInternalBegin(pCtx);
  if (!pCtx->pLogStmt) {
    rc = sqlite3_prepare_v3(pCtx->db,
      "INSERT INTO _hpplite_pending(txn, sql, mode, now) VALUES(?1, ?2, ?3, ?4)",
      -1, SQLITE_PREPARE_PERSISTENT, &pCtx->pLogStmt, NULL);
    if (rc != SQLITE_OK) pCtx->pLogStmt = NULL;
  }
  if (pCtx->pLogStmt) {
    /* A transaction is named by the id of its first logged row (stored as 0
    ** there, "= id"): unique across every connection and process writing
    ** this file, with no counter to keep in step (issue #24). */
    sqlite3_bind_int64(pCtx->pLogStmt, 1, pCtx->curTxn);
    sqlite3_bind_text(pCtx->pLogStmt, 2, zExpanded, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(pCtx->pLogStmt, 3, currentMode(pCtx));
    sqlite3_bind_int64(pCtx->pLogStmt, 4, slot >= 0 ? pCtx->aRun[slot].now : 0);
    {
      /* our row's id is not the application's last_insert_rowid() */
      sqlite3_int64 appRowid = sqlite3_last_insert_rowid(pCtx->db);
      rc = hpplite_step_write(pCtx->pLogStmt);
      if (rc == SQLITE_DONE) pCtx->logged.id = sqlite3_last_insert_rowid(pCtx->db);
      sqlite3_set_last_insert_rowid(pCtx->db, appRowid);
    }
    if (rc == SQLITE_DONE) {
      rerunBegin(pCtx, pS);
      pCtx->logged.pS = pS;
      pCtx->logged.keep = isHeaderPragmaWrite(zSql);
      if (slot < 0) slot = runFind(pCtx, pS);
      if (slot >= 0) pCtx->aRun[slot].logId = pCtx->logged.id;
      if (pCtx->curTxn == 0) pCtx->curTxn = pCtx->logged.id;
      pCtx->logged.changes = totalChanges(pCtx->db);
      pCtx->logged.schema = hppliteSchemaVersion(pCtx);
      {
        const char *z = zSql;
        while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
        pCtx->logged.bSeq = (sqlite3_strnicmp(z, "INSERT", 6) == 0 || sqlite3_strnicmp(z, "REPLACE", 7) == 0
                           || sqlite3_strnicmp(z, "WITH", 4) == 0)
                          && seqFingerprint(pCtx, pCtx->logged.seq);
      }
    }
    sqlite3_reset(pCtx->pLogStmt);
    sqlite3_clear_bindings(pCtx->pLogStmt);
  } else {
    rc = SQLITE_ERROR;
  }
  hppliteInternalEnd(pCtx, saved);
  /* the statement's own clock reads start after our bookkeeping above */
  if (slot >= 0) pCtx->aRun[slot].clockSeen = hpplite_clock_consulted();
  if (rc != SQLITE_DONE) {
    char why[160];
    snprintf(why, sizeof(why), "could not log statement (%s)", sqlite3_errmsg(pCtx->db));
    /* the log row could not be written because the connection's schema is
    ** stale and stays so while this statement runs (#108): captured at its
    ** first run's end instead, when it can be */
    if ((rc & 0xff) != SQLITE_SCHEMA || !deferCapture(pCtx, pS, 1)) refuseStatement(pCtx, pS, why, zSql);
  }
  sqlite3_free(zExpanded);
  return 0;
}

int hppliteTraceCallback(unsigned mask, void *pArg, void *pStmt, void *pX) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  sqlite3_stmt *pS = (sqlite3_stmt*)pStmt;
  const char *zSql = (const char*)pX;
  int slot = -1;
  ActiveRun done = { 0, 0, 0, 0 };

  if (!pCtx || pCtx->bInternal) return 0;
  /* Mid-step bookkeeping for hppliteBkBegin(): the timer thread must not ask the
  ** connection itself (sqlite3_next_stmt takes the connection mutex, which
  ** sqlite3_close holds while it joins the timer thread - a deadlock). */
  if (mask == SQLITE_TRACE_STMT && pCtx->rerunStage == 2 && pS != pCtx->pRerunStmt
      && !(zSql && zSql[0] == '-' && zSql[1] == '-')) {
    /* the rerun never came (its first run failed otherwise). Not on a
    ** sub-program's trace: a CREATE VIRTUAL TABLE's nested DDL runs inside
    ** the rerun, which is in progress - a COMMIT there fails, and the
    ** transaction stayed open with every later statement in it (#95) */
    rerunCommit(pCtx);
  }
  if (mask == SQLITE_TRACE_STMT) { pCtx->pRefusedStmt = NULL; pCtx->bDenyReprepare = 0; }   /* a new statement: the refused one ran or failed */
  if (mask == SQLITE_TRACE_STMT) {
    /* Sub-programs are traced too and have no PROFILE event of their own.
    ** Named triggers say so ("-- TRIGGER x"); foreign key actions (ON
    ** DELETE CASCADE ...) are unnamed and arrive with the text of the
    ** statement that fired them, for a statement already running. Neither
    ** is a statement of its own: not counted, not logged (#52).
    ** Should a run's end ever go unreported, its next run would arrive as a
    ** trace for a "running" statement and be taken for a sub-program. SQLite
    ** adds to the VM step count only when sqlite3_step() returns, and a write
    ** does all its writing in its first step (RETURNING included): a
    ** sub-program sees the count its statement started with, a new run a
    ** larger one. */
    int running;
    if (zSql && zSql[0] == '-' && zSql[1] == '-') return 0;
    if (pCtx->nRun > 0) endResetRuns(pCtx, pS);
    running = runFind(pCtx, pS);
    if (running >= 0) {
      if (sqlite3_stmt_status(pS, SQLITE_STMTSTATUS_VM_STEP, 0) == pCtx->aRun[running].vmSteps) return 0;
      endActiveRun(pCtx, running);          /* the last run was reset unfinished: this is a new one */
    }
    slot = runStart(pCtx, pS, 0, 0);
    pCtx->bAppTxnOpen = !sqlite3_get_autocommit(pCtx->db);
  } else if (mask == SQLITE_TRACE_PROFILE) {
    int i;
    /* a commit whose hook fired is visible once its statement ends (#37) */
    __atomic_store_n(&pCtx->nCommitDone, __atomic_load_n(&pCtx->nCommitBegun, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    i = runFind(pCtx, pS);
    if (i >= 0) done = endActiveRun(pCtx, i);
    if (!done.found && pCtx->nAppStmtActive > 0) pCtx->nAppStmtActive--;
    pCtx->bAppTxnOpen = !sqlite3_get_autocommit(pCtx->db);
    gateLeaveAppIfIdle(pCtx);
  }
  if (!pCtx->bEnabled) return 0;
  if (pCtx->bStatusGone && mask == SQLITE_TRACE_STMT) {
    /* A close that failed (SQLITE_BUSY) disconnected hpplite_status; the
    ** next successful close must flush again, which needs the instance back */
    pCtx->bStatusGone = 0;
    hppliteInternalExec(pCtx, pCtx->db, "SELECT * FROM hpplite_status LIMIT 0");
  }
  if (mask == SQLITE_TRACE_PROFILE) {
    int bWasLogged = pS == pCtx->logged.pS;
    /* (not at the failed first run of a tracked statement: its rerun follows
    ** inside this step, in the transaction; its end commits) */
    if (pCtx->ownTxn == HPPLITE_OWN_COMMIT_PENDING && !(pS == pCtx->pRerunStmt && pCtx->rerunStage == 1 && stmtExpired(pS)))
      rerunCommitRetry(pCtx);
    /* a refused statement done - unless this is its first run failing stale
    ** (expired: SQLite reprepares and reruns it inside this step): the rerun
    ** must be refused too (#89) */
    if (pS == pCtx->pRefusedStmt && !stmtExpired(pS)) pCtx->pRefusedStmt = NULL;
    /* ...and one HPPLite tracks for its rerun is known to be reprepared next,
    ** whatever the error code says after HPPLite's own statements ran here */
    if (pS == pCtx->pRefusedStmt && pS == pCtx->pRerunStmt && pCtx->rerunStage == 1) pCtx->bDenyReprepare = 1;
    if (pS == pCtx->pVacuumStmt) {
      /* Done: rowids of tables without an INTEGER PRIMARY KEY may have been
      ** renumbered, and statements and records after it address the new
      ** ones - a replica must VACUUM at this point of the log too. Logged
      ** now, not as it started (VACUUM cannot run in the transaction a log
      ** row would open), and only when it ran (it bumps schema_version). */
      pCtx->pVacuumStmt = NULL;
      if (pCtx->vacuumSchema >= 0 && hppliteSchemaVersion(pCtx) != pCtx->vacuumSchema) logVacuum(pCtx);
    }
    if (pS == pCtx->pRerunStmt && pCtx->rerunStage == 1 && stmtExpired(pS)) {
      /* the failed first run of a stale statement: no effect to check - the
      ** rerun follows, and its end checks (and commits) */
      rerunStatementDone(pCtx, pS);
      return 0;
    }
    hppliteStatementDone(pCtx, pS);
    /* a schema change of this connection's own is not staleness */
    if (bWasLogged && pS != pCtx->pRerunStmt) pCtx->seenSchema = hppliteSchemaVersion(pCtx);
    if (pS == pCtx->pRerunStmt) rerunStatementDone(pCtx, pS);
    if (pCtx->bRows) {
      if (pS == pCtx->pRowsTextStmt) { pCtx->pRowsTextStmt = NULL; pCtx->bRowsCapOff = 0; }
      /* a safe point for DDL: the statement is over (not a reset mid-run).
      ** Only after a write: a read cannot move the schema, and another
      ** connection's change is caught at the next write (text, then this) */
      /* ...and not a first run that failed stale (SQLITE_SCHEMA): SQLite
      ** reruns it now, and triggers rebuilt in between would be the ones it
      ** was rid of (a DROP COLUMN, below). A schema change that ran expires
      ** itself too: that one is the reason to rebuild */
      /* (expired with SQLITE_SCHEMA: that failed stale run. The error code
      ** alone is not it: HPPLite's own statements leave it there too, when
      ** the schema was stale and pinned at the statement's start, #108) */
      if (!sqlite3_stmt_readonly(pS) && !sqlite3_stmt_busy(pS) && pCtx->nRun == 0
          && !(stmtExpired(pS) && sqlite3_errcode(pCtx->db) == SQLITE_SCHEMA))
        hpplite_rows_after_statement(pCtx);
    }
    /* Its row carries the time pinned for it (#56). One clock read is
    ** SQLite's own: with PROFILE tracing on it times every statement, the
    ** closing read just before this callback. With no other read, the
    ** batch can leave the time out. No write here: a write now would
    ** become what changes() reports. */
    if (done.now && done.log && done.reads <= 1) noteNoClock(pCtx, done.log, done.now);
    return 0;
  }
  if (mask != SQLITE_TRACE_STMT || !zSql) return 0;
  /* SQLite's own verdict: a statement that cannot write is never logged, so
  ** nothing in it needs to replay (WITH ... SELECT random() included) */
  if (pS && sqlite3_stmt_readonly(pS)) return 0;
  /* Row capture: a column dropped from a table our triggers name it in */
  if (pCtx->bRows) {
    char zTab[256];
    if (hpplite_rows_drop_column_target(zSql, zTab, (int)sizeof(zTab))) {
      /* Dropping triggers expires this statement: SQLite reruns it inside
      ** this step (as for any stale statement). In autocommit the failed
      ** first run would roll the drop back with it, so the statement runs
      ** in a transaction of HPPLite's, committed at its end (the rerun
      ** machinery of a07891ef34) */
      rerunTrack(pCtx, pS);
      hpplite_rows_drop_triggers_for(pCtx, zTab);
    }
  }
  {
    const char *z = zSql;
    while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
    pCtx->zDropTable[0] = 0;              /* prepared already: the drop it allowed is over */
    if (sqlite3_strnicmp(z, "VACUUM", 6) == 0 && !isalnum((unsigned char)z[6]) && z[6] != '_') {
      /* VACUUM INTO writes a copy elsewhere: this database is unchanged */
      pCtx->pVacuumStmt = pS;
      pCtx->vacuumSchema = strcasestr(z, "INTO") ? -1 : hppliteSchemaVersion(pCtx);
    }
  }
  return captureStart(pCtx, pS, zSql, slot);
}

/*
** Commit hook: close the current transaction group.
*/
int hppliteCommitHook(void *pArg) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  if (pCtx->bInternal) return 0;
  pCtx->curTxn = 0;
  pCtx->bRootDirty = 1;
  __atomic_add_fetch(&pCtx->nCommitBegun, 1, __ATOMIC_RELEASE);
  if (pCtx->xOnCommit) {
    pCtx->xOnCommit(pCtx->pCallbackArg, NULL, 0);
  }
  return 0;  /* Allow commit */
}

/*
** Rollback hook: pending rows written in the transaction are gone with it.
*/
void hppliteRollbackHook(void *pArg) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  if (pCtx->bInternal) return;
  pCtx->curTxn = 0;
  if (pCtx->xOnRollback) {
    pCtx->xOnRollback(pCtx->pCallbackArg);
  }
}
