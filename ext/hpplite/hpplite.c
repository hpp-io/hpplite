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
static int internalExec(HppliteCtx *pCtx, sqlite3 *db, const char *zSql) {
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

/*
** ============================================================================
** Hooks
** ============================================================================
*/

/*
** Decide whether a statement is a write worth logging, by its first keyword.
*/
/* PRAGMA [schema.]user_version / application_id = N (or (N)): the two header
** fields an application writes - migration tools keep their version there.
** Deterministic, so captured and replayed like any write; their reads are not. */
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
static void pragmaModeRead(HppliteCtx *pCtx) {
  static const char *aRead[] = { "legacy_alter_table", "writable_schema", "ignore_check_constraints", NULL };
  int i, saved = pCtx->bInternal;
  pCtx->bInternal = 1;
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
  pCtx->bInternal = saved;
}

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

static int schemaVersion(HppliteCtx *pCtx) {
  int v = -1, savedInternal = pCtx->bInternal;
  pCtx->bInternal = 1;                   /* ours: its trace must not pass for the application's next statement */
  if (!pCtx->pSchemaStmt) {
    if (sqlite3_prepare_v3(pCtx->db, "PRAGMA schema_version", -1, SQLITE_PREPARE_PERSISTENT,
                           &pCtx->pSchemaStmt, NULL) != SQLITE_OK) {
      pCtx->pSchemaStmt = NULL;
      pCtx->bInternal = savedInternal;
      return -1;
    }
  }
  if (sqlite3_step(pCtx->pSchemaStmt) == SQLITE_ROW) v = sqlite3_column_int(pCtx->pSchemaStmt, 0);
  sqlite3_reset(pCtx->pSchemaStmt);
  pCtx->bInternal = savedInternal;
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
static void hppliteStatementDone(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  if (pS != pCtx->pLastLogged) return;
  pCtx->pLastLogged = NULL;
  if (pCtx->bLastLoggedKeep) return;   /* a header pragma changes neither rows nor schema: keep */
  /* Reset before it finished (a RETURNING write stepped once - node:sqlite's
  ** .get()): SQLite reports the end from sqlite3_reset() while the statement
  ** is still busy, before it counts the changes it made. Its writes are done
  ** (they all happen in the first step), the count says nothing: keep. */
  if (sqlite3_stmt_busy(pS)) return;

  pCtx->bInternal = 1;
  if (totalChanges(pCtx->db) != pCtx->changesAtLog
      || schemaVersion(pCtx) != pCtx->schemaAtLog) {
    pCtx->bInternal = 0;
    return;   /* had an effect: keep */
  }
  if (pCtx->bSeqAtLog) {
    sqlite3_int64 now[2];
    if (seqFingerprint(pCtx, now) && (now[0] != pCtx->seqAtLog[0] || now[1] != pCtx->seqAtLog[1])) {
      pCtx->bInternal = 0;
      return;   /* no row changed, but sqlite_sequence did: keep */
    }
  }
  /* No effect. Inside a transaction its row goes before anyone can see it:
  ** a statement that failed there must not reach the chain (in autocommit a
  ** failed statement's rollback took the row already), and a no-op need not.
  ** In autocommit the row committed with the statement and may already be in
  ** a snapshot - a batch being built in this process or another; removing it
  ** now would leave that batch carrying a row the log no longer has. It stays:
  ** a statement that changed nothing on the writer changes nothing on a
  ** replica with the same state. */
  if (sqlite3_get_autocommit(pCtx->db)) {
    pCtx->bInternal = 0;
    return;
  }
  if (!pCtx->pUnlogStmt) {
    if (sqlite3_prepare_v3(pCtx->db, "DELETE FROM _hpplite_pending WHERE id = ?1",
                           -1, SQLITE_PREPARE_PERSISTENT, &pCtx->pUnlogStmt, NULL) != SQLITE_OK) {
      pCtx->pUnlogStmt = NULL;
    }
  }
  if (pCtx->pUnlogStmt) {
    sqlite3_bind_int64(pCtx->pUnlogStmt, 1, pCtx->lastLogId);
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
  pCtx->bInternal = 0;
}

/*
** Replay re-executes SQL text, so a statement whose result depends on
** anything but the database (random values, the clock, the connection) is
** not reproducible: the sequencer's rows and a verifier's rows differ and
** the root check fails. Spot the usual suspects and count them, so a user
** can see why verification would fail (hpplite_status.nondeterministic).
*/
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
static void freeFunctionChecks(HppliteCtx *pCtx) {
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
      snprintf(pCtx->zCollectWhy, sizeof(pCtx->zCollectWhy),
               "non-deterministic function %s() in a write; replay cannot reproduce it", lower);
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
/* Returns the reason the write cannot replay, or NULL */
static const char *functionVerdict(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  const char *zSql = sqlite3_sql(pS);
  struct HppliteFnVerdict *v;
  sqlite3_stmt *p = NULL;
  int schema, rc;
  uint64_t h = 1469598103934665603ULL;
  const char *c;
  char *zShape;
  if (!zSql) return NULL;
  zShape = statementShape(zSql);
  if (!zShape) {
    snprintf(pCtx->zCollectWhy, sizeof(pCtx->zCollectWhy), "could not check the functions of a write (out of memory)");
    return pCtx->zCollectWhy;
  }
  for (c = zShape; *c; c++) h = (h ^ (unsigned char)*c) * 1099511628211ULL;
  /* The key must not read the file: a read of our own here, while the
  ** application statement is starting, races a bookkeeping commit (WAL
  ** snapshot, see bkBegin). The data version is in memory and moves when
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
    return v->zWhy[0] ? v->zWhy : NULL;
  }
  pCtx->zCollectWhy[0] = 0;
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
  memcpy(v->zWhy, pCtx->zCollectWhy, sizeof(v->zWhy));
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
static int hppliteAuthorizer(void *pArg, int op, const char *z1, const char *z2, const char *z3, const char *z4) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  int rc;
  (void)z3;
  if (!pCtx) return SQLITE_OK;
  if (pCtx->bCollectFns) {
    /* the capture triggers' functions are ours, and silent in a text statement */
    if (pCtx->bRows && z4 && sqlite3_strnicmp(z4, "hpplite_cap_", 12) == 0) return SQLITE_OK;
    if (op == SQLITE_FUNCTION && z2 && !pCtx->zCollectWhy[0]) collectFunction(pCtx, z2);
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
    int bReprepare = sqlite3_errcode(pCtx->db) == SQLITE_SCHEMA;
    pCtx->pRefusedStmt = NULL;
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

/* A run of an application statement is over: forget it (PROFILE, or found
** reset without finishing). Returns what was recorded for it. */
typedef struct ActiveRun { sqlite3_int64 log, now; unsigned reads; int found; } ActiveRun;
static ActiveRun endActiveRun(HppliteCtx *pCtx, int i) {
  ActiveRun r;
  int last = --pCtx->nActiveStmt;
  r.log = pCtx->aActiveLog[i]; r.now = pCtx->aActiveNow[i]; r.found = 1;
  /* clock reads during the run, counted before the pin goes (it resets the
  ** count) and before our own statements below read it */
  r.reads = hpplite_clock_consulted() - pCtx->aActiveSeen[i];
  if (pCtx->aActiveStmt[i] == pCtx->pPinOwner) {
    hpplite_clock_pin(0);
    pCtx->pPinOwner = NULL;
  }
  pCtx->aActiveStmt[i] = pCtx->aActiveStmt[last];
  pCtx->aActiveLog[i] = pCtx->aActiveLog[last];
  pCtx->aActiveNow[i] = pCtx->aActiveNow[last];
  pCtx->aActiveSeen[i] = pCtx->aActiveSeen[last];
  pCtx->aActiveVm[i] = pCtx->aActiveVm[last];
  if (pCtx->nAppStmtActive > 0) pCtx->nAppStmtActive--;
  return r;
}

/* Runs whose end went unreported (finalized while tracing was replaced,
** say) must not count as running forever - bookkeeping would never commit:
** runs that are no longer busy, or no longer exist, are over. Application
** thread. */
static void endResetRuns(HppliteCtx *pCtx, sqlite3_stmt *pExcept) {
  int j;
  for (j = pCtx->nActiveStmt - 1; j >= 0; j--) {
    sqlite3_stmt *p = NULL, *q = pCtx->aActiveStmt[j];
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
    fprintf(stderr, "hpplite: rerun-tracking (schema %d, seen %d, autocommit %d): %.80s\n", schemaVersion(pCtx),
            pCtx->seenSchema, sqlite3_get_autocommit(pCtx->db), sqlite3_sql(pS) ? sqlite3_sql(pS) : "?");
  if (sqlite3_get_autocommit(pCtx->db)) {
    if (internalExec(pCtx, pCtx->db, "BEGIN") != SQLITE_OK) return 0;
    pCtx->rerunOwnTxn = 1;
  } else {
    pCtx->rerunOwnTxn = 0;
  }
  pCtx->pRerunStmt = pS;
  pCtx->rerunStage = 1;
  pCtx->rerunSchema = schemaVersion(pCtx);
  return 1;
}
static void rerunBegin(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  int v;
  if (pCtx->pRerunStmt) return;
  v = schemaVersion(pCtx);
  if (v >= 0 && pCtx->seenSchema >= 0 && v != pCtx->seenSchema) rerunTrack(pCtx, pS);
  pCtx->seenSchema = v;
}
static void rerunCommit(HppliteCtx *pCtx) {
  int own = pCtx->rerunOwnTxn;
  pCtx->pRerunStmt = NULL;
  pCtx->rerunStage = 0;
  pCtx->rerunOwnTxn = 0;
  if (own && !sqlite3_get_autocommit(pCtx->db)) {
    if (internalExec(pCtx, pCtx->db, "COMMIT") == SQLITE_OK) {
      /* what the commit hook does for an application commit (ours it skips) */
      pCtx->curTxn = 0;
      pCtx->bRootDirty = 1;
      if (pCtx->xOnCommit) pCtx->xOnCommit(pCtx->pCallbackArg, NULL, 0);
    } else {
      /* a statement in progress (this is its trace): ours stays open with
      ** the application's statement inside, and is committed at that
      ** statement's end - never left open past it */
      pCtx->rerunCommitPending = 1;
    }
  }
  pCtx->bAppTxnOpen = !sqlite3_get_autocommit(pCtx->db);
  gateLeaveAppIfIdle(pCtx);
}
/* ...that end: the application's statement over, nothing is in progress */
static void rerunCommitRetry(HppliteCtx *pCtx) {
  if (sqlite3_get_autocommit(pCtx->db)) { pCtx->rerunCommitPending = 0; return; }   /* ended by the application */
  if (internalExec(pCtx, pCtx->db, "COMMIT") == SQLITE_OK) {
    pCtx->rerunCommitPending = 0;
    pCtx->curTxn = 0;
    pCtx->bRootDirty = 1;
    if (pCtx->xOnCommit) pCtx->xOnCommit(pCtx->pCallbackArg, NULL, 0);
    pCtx->bAppTxnOpen = 0;
    gateLeaveAppIfIdle(pCtx);
  } else {
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
/* PROFILE of that statement: its first run failed and expired it - the rerun
** follows (running again, as a STMT trace would have recorded it); or it is
** over (it ran, or failed for another reason): commit */
static void rerunStatementDone(HppliteCtx *pCtx, sqlite3_stmt *pS) {
  /* expired by a schema change of its own (a CREATE VIRTUAL TABLE's nested
  ** DDL expires every statement, this one too): it ran, no rerun follows */
  if (pCtx->rerunStage == 1 && stmtExpired(pS) && schemaVersion(pCtx) == pCtx->rerunSchema) {
    pCtx->rerunStage = 2;
    if (pCtx->nActiveStmt < (int)(sizeof(pCtx->aActiveStmt) / sizeof(pCtx->aActiveStmt[0]))) {
      int slot = pCtx->nActiveStmt++;
      pCtx->aActiveStmt[slot] = pS;
      pCtx->aActiveLog[slot] = pCtx->lastLogId;
      pCtx->aActiveNow[slot] = 0;
      pCtx->aActiveSeen[slot] = hpplite_clock_consulted();
      pCtx->aActiveVm[slot] = sqlite3_stmt_status(pS, SQLITE_STMTSTATUS_VM_STEP, 0);
      gateEnterApp(pCtx);
      pCtx->nAppStmtActive++;
    }
    pCtx->pLastLogged = pS;               /* the effect check applies to the rerun */
    return;
  }
  rerunCommit(pCtx);
  pCtx->seenSchema = schemaVersion(pCtx);
}

/* A VACUUM that ran, into the log as a group of its own; the application's
** last_insert_rowid() is kept (VACUUM is not a statement whose changes()
** anyone reads) */
static void logVacuum(HppliteCtx *pCtx) {
  sqlite3_stmt *q = NULL;
  sqlite3_int64 appRowid = sqlite3_last_insert_rowid(pCtx->db);
  int saved = pCtx->bInternal;
  pCtx->bInternal = 1;
  if (sqlite3_prepare_v2(pCtx->db, "INSERT INTO _hpplite_pending(txn, sql, mode, now) VALUES(0, 'VACUUM', 0, 0)",
                         -1, &q, NULL) == SQLITE_OK) {
    if (hpplite_step_write(q) != SQLITE_DONE) pCtx->nLogErrors++;
  } else {
    pCtx->nLogErrors++;
  }
  sqlite3_finalize(q);
  sqlite3_set_last_insert_rowid(pCtx->db, appRowid);
  pCtx->bInternal = saved;
}

static int hppliteTraceCallback(unsigned mask, void *pArg, void *pStmt, void *pX) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  sqlite3_stmt *pS = (sqlite3_stmt*)pStmt;
  const char *zSql = (const char*)pX;
  char *zExpanded;
  int rc, slot = -1;
  ActiveRun done = { 0, 0, 0, 0 };

  if (!pCtx || pCtx->bInternal) return 0;
  /* Mid-step bookkeeping for bkBegin(): the timer thread must not ask the
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
  if (mask == SQLITE_TRACE_STMT) pCtx->pRefusedStmt = NULL;      /* a new statement: the refused one ran or failed */
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
    int i, running = -1;
    if (zSql && zSql[0] == '-' && zSql[1] == '-') return 0;
    if (pCtx->nActiveStmt > 0) endResetRuns(pCtx, pS);
    for (i = 0; i < pCtx->nActiveStmt; i++) if (pCtx->aActiveStmt[i] == pS) { running = i; break; }
    if (running >= 0) {
      if (sqlite3_stmt_status(pS, SQLITE_STMTSTATUS_VM_STEP, 0) == pCtx->aActiveVm[running]) return 0;
      endActiveRun(pCtx, running);          /* the last run was reset unfinished: this is a new one */
    }
    if (pCtx->nActiveStmt < (int)(sizeof(pCtx->aActiveStmt) / sizeof(pCtx->aActiveStmt[0]))) {
      slot = pCtx->nActiveStmt++;
      pCtx->aActiveStmt[slot] = pS;
      pCtx->aActiveLog[slot] = 0;
      pCtx->aActiveNow[slot] = 0;
      pCtx->aActiveSeen[slot] = 0;
      pCtx->aActiveVm[slot] = sqlite3_stmt_status(pS, SQLITE_STMTSTATUS_VM_STEP, 0);
    }
    gateEnterApp(pCtx);
    pCtx->nAppStmtActive++;
    pCtx->bAppTxnOpen = !sqlite3_get_autocommit(pCtx->db);
  } else if (mask == SQLITE_TRACE_PROFILE) {
    int i;
    /* a commit whose hook fired is visible once its statement ends (#37) */
    __atomic_store_n(&pCtx->nCommitDone, __atomic_load_n(&pCtx->nCommitBegun, __ATOMIC_ACQUIRE), __ATOMIC_RELEASE);
    for (i = 0; i < pCtx->nActiveStmt; i++) {
      if (pCtx->aActiveStmt[i] == pS) { done = endActiveRun(pCtx, i); break; }
    }
    if (!done.found && pCtx->nAppStmtActive > 0) pCtx->nAppStmtActive--;
    pCtx->bAppTxnOpen = !sqlite3_get_autocommit(pCtx->db);
    gateLeaveAppIfIdle(pCtx);
  }
  if (!pCtx->bEnabled) return 0;
  if (pCtx->bStatusGone && mask == SQLITE_TRACE_STMT) {
    /* A close that failed (SQLITE_BUSY) disconnected hpplite_status; the
    ** next successful close must flush again, which needs the instance back */
    pCtx->bStatusGone = 0;
    internalExec(pCtx, pCtx->db, "SELECT * FROM hpplite_status LIMIT 0");
  }
  if (mask == SQLITE_TRACE_PROFILE) {
    int bWasLogged = pS == pCtx->pLastLogged;
    if (pCtx->rerunCommitPending) rerunCommitRetry(pCtx);
    /* a refused statement done - unless this is its first run failing stale
    ** (expired: SQLite reprepares and reruns it inside this step): the rerun
    ** must be refused too (#89) */
    if (pS == pCtx->pRefusedStmt && !stmtExpired(pS)) pCtx->pRefusedStmt = NULL;
    if (pS == pCtx->pVacuumStmt) {
      /* Done: rowids of tables without an INTEGER PRIMARY KEY may have been
      ** renumbered, and statements and records after it address the new
      ** ones - a replica must VACUUM at this point of the log too. Logged
      ** now, not as it started (VACUUM cannot run in the transaction a log
      ** row would open), and only when it ran (it bumps schema_version). */
      pCtx->pVacuumStmt = NULL;
      if (pCtx->vacuumSchema >= 0 && schemaVersion(pCtx) != pCtx->vacuumSchema) logVacuum(pCtx);
    }
    if (pS == pCtx->pRerunStmt && pCtx->rerunStage == 1 && stmtExpired(pS)) {
      /* the failed first run of a stale statement: no effect to check - the
      ** rerun follows, and its end checks (and commits) */
      rerunStatementDone(pCtx, pS);
      return 0;
    }
    hppliteStatementDone(pCtx, pS);
    /* a schema change of this connection's own is not staleness */
    if (bWasLogged && pS != pCtx->pRerunStmt) pCtx->seenSchema = schemaVersion(pCtx);
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
      if (!sqlite3_stmt_readonly(pS) && !sqlite3_stmt_busy(pS) && pCtx->nActiveStmt == 0
          && sqlite3_errcode(pCtx->db) != SQLITE_SCHEMA)
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
      pCtx->vacuumSchema = strcasestr(z, "INTO") ? -1 : schemaVersion(pCtx);
    }
  }
  /* Row capture (#19): the triggers record what a write changes - its text,
  ** its functions and the clock no longer matter. A write on a table they do
  ** not cover yet (created by another connection since the last rebuild) is
  ** logged as text, the v1 way, its triggers silent; rebuilt after it. So
  ** is a write that may reach a virtual table (FTS5, R*Tree): what those
  ** store cannot carry triggers. */
  if (pCtx->bRows) {
    int covered = isRowsDml(zSql) ? hpplite_rows_covered_for_write(pCtx) : 0;
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
      hpplite_rows_after_statement(pCtx);            /* the rebuild, when the triggers are not the snapshot's */
      if (hpplite_rows_covered(pCtx)) {
        pCtx->changesAtLog = -1;                     /* no text row: the rerun's effect check keeps nothing */
        pCtx->bSeqAtLog = 0;
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
  /* Statements replay cannot reproduce are refused when logged as text
  ** (every write under capture=text; with row capture a schema change, a
  ** write reaching a virtual table, or a table another connection just
  ** created, #19): connection state that changes what a later statement
  ** does, and non-deterministic values. Refused by default so the
  ** application learns at the write, not from a root mismatch 100 batches
  ** later; determinism=warn keeps the old warn-and-count behaviour. The
  ** refusal says why this statement is text, so the rule is not taken for
  ** row capture's (#85). */
  const char *zAsText = pCtx->bRows
    ? " - this statement is logged as text (a schema change, a write reaching a virtual table, or a table another"
      " connection just created) and replays as written"
    : "";
  {
    char *lower = lowercaseDup(zSql);
    const char *zWhy = NULL;
    if (lower) {
      /* the clock replays when the connection is on the clock VFS (#56);
      ** a machine's time zone never does */
      if (isTrackedSql(zSql) && usesTimeZone(lower))
        zWhy = "'localtime'/'utc' depend on the machine's time zone, which differs between replicas";
      else if (isTrackedSql(zSql) && !pCtx->bClockPin && isNondeterministicSql(lower))
        zWhy = "non-deterministic SQL (random(), 'now', CURRENT_TIMESTAMP, date() ...) cannot be replayed";
    }
    sqlite3_free(lower);
    if (zWhy) {
      pCtx->nNondeterministic++;
      if (!pCtx->bDeterminismWarn) {
        char why[400];
        snprintf(why, sizeof(why), "%s%s; bind the value instead", zWhy, zAsText);
        refuseStatement(pCtx, pS, why, zSql);
        return 0;
      }
      if (pCtx->nNondeterministic == 1) {
        fprintf(stderr, "hpplite: %s; verification will report a mismatch: %.80s\n", zWhy, zSql);
      }
    }
  }
  if (!isTrackedSql(zSql)) return 0;
  if (mentionsHppliteTable(zSql)) {
    /* A write that names our tables where the authorizer could not see it
    ** (RENAME TO _hpplite_x, a subquery over the log whose rows differ on
    ** every replica): not logged, so not allowed either. */
    refuseStatement(pCtx, pS, "application writes may not reference _hpplite_ tables", zSql);
    return 0;
  }
  {
    const char *z = zSql;
    while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
    if (sqlite3_strnicmp(z, "CREATE", 6) == 0 || sqlite3_strnicmp(z, "DROP", 4) == 0
        || sqlite3_strnicmp(z, "ALTER", 5) == 0) {
      pCtx->nDdlGen++;                    /* views and triggers change what a write calls */
    }
  }
  {
    const char *zWhy = functionVerdict(pCtx, pS);
    if (zWhy && strncmp(zWhy, "could not check", 15) == 0) {
      /* the check itself failed (out of memory): refused, nothing to advise */
      refuseStatement(pCtx, pS, zWhy, zSql);
      return 0;
    }
    if (zWhy) {
      int bAppFn = strstr(zWhy, "defined by the application") != NULL;
      char why[400];
      pCtx->nNondeterministic++;
      /* determinism=warn lets non-deterministic values through (counted);
      ** a function the replica does not have never replays at all */
      if (!pCtx->bDeterminismWarn || bAppFn) {
        snprintf(why, sizeof(why), "%s%s; compute the value in the application and bind it", zWhy, zAsText);
        refuseStatement(pCtx, pS, why, zSql);
        return 0;
      }
    }
  }

  /* A SQLite that cuts bound values in the expansion (below) is used with
  ** row capture only (#62): the few statements still logged as text are
  ** refused when they bind anything, which would replay cut */
  if (pCtx->bExpandTruncates && pS && sqlite3_bind_parameter_count(pS) > 0) {
    refuseStatement(pCtx, pS, "this SQLite truncates bound values in sqlite3_expanded_sql() (better-sqlite3's "
                    "bundled SQLite), and this statement is logged as text (a schema change, a write that "
                    "reaches a virtual table, or the first write to a table another connection just "
                    "created): put the values in its text, or run it on a stock SQLite", zSql);
    return 0;
  }
  /* Bound parameters are substituted so the statement can be replayed */
  zExpanded = sqlite3_expanded_sql(pS);
  if (!zExpanded) {
    refuseStatement(pCtx, pS, "cannot expand bound parameters (out of memory?)", zSql);
    return 0;
  }
  /* No size limit here: a transaction larger than a batch spans several
  ** (#57), and so does a statement - its text is posted in pieces (#59) */

  if (pCtx->bClockPin && slot < 0) {
    int i;
    for (i = 0; i < pCtx->nActiveStmt; i++) if (pCtx->aActiveStmt[i] == pS) { slot = i; break; }
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
    pCtx->aActiveNow[slot] = t;
  }

  pCtx->bInternal = 1;
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
    sqlite3_bind_int64(pCtx->pLogStmt, 4, slot >= 0 ? pCtx->aActiveNow[slot] : 0);
    {
      /* our row's id is not the application's last_insert_rowid() */
      sqlite3_int64 appRowid = sqlite3_last_insert_rowid(pCtx->db);
      rc = hpplite_step_write(pCtx->pLogStmt);
      if (rc == SQLITE_DONE) pCtx->lastLogId = sqlite3_last_insert_rowid(pCtx->db);
      sqlite3_set_last_insert_rowid(pCtx->db, appRowid);
    }
    if (rc == SQLITE_DONE) {
      rerunBegin(pCtx, pS);
      pCtx->pLastLogged = pS;
      pCtx->bLastLoggedKeep = isHeaderPragmaWrite(zSql);
      if (slot < 0) {
        int i;
        for (i = 0; i < pCtx->nActiveStmt; i++) if (pCtx->aActiveStmt[i] == pS) { slot = i; break; }
      }
      if (slot >= 0) pCtx->aActiveLog[slot] = pCtx->lastLogId;
      if (pCtx->curTxn == 0) pCtx->curTxn = pCtx->lastLogId;
      pCtx->changesAtLog = totalChanges(pCtx->db);
      pCtx->schemaAtLog = schemaVersion(pCtx);
      {
        const char *z = zSql;
        while (*z == ' ' || *z == '\t' || *z == '\n' || *z == '\r') z++;
        pCtx->bSeqAtLog = (sqlite3_strnicmp(z, "INSERT", 6) == 0 || sqlite3_strnicmp(z, "REPLACE", 7) == 0
                           || sqlite3_strnicmp(z, "WITH", 4) == 0)
                          && seqFingerprint(pCtx, pCtx->seqAtLog);
      }
    }
    sqlite3_reset(pCtx->pLogStmt);
    sqlite3_clear_bindings(pCtx->pLogStmt);
  } else {
    rc = SQLITE_ERROR;
  }
  pCtx->bInternal = 0;
  /* the statement's own clock reads start after our bookkeeping above */
  if (slot >= 0) pCtx->aActiveSeen[slot] = hpplite_clock_consulted();
  if (rc != SQLITE_DONE) {
    char why[160];
    snprintf(why, sizeof(why), "could not log statement (%s)", sqlite3_errmsg(pCtx->db));
    refuseStatement(pCtx, pS, why, zSql);
  }
  sqlite3_free(zExpanded);
  return 0;
}

/*
** Commit hook: close the current transaction group.
*/
static int hppliteCommitHook(void *pArg) {
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
static void hppliteRollbackHook(void *pArg) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  if (pCtx->bInternal) return;
  pCtx->curTxn = 0;
  if (pCtx->xOnRollback) {
    pCtx->xOnRollback(pCtx->pCallbackArg);
  }
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
    rc = internalExec(pCtx, db, z);
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

  rc = internalExec(pCtx, db, HPPLITE_SCHEMA);
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
    if (!has) rc = internalExec(pCtx, db, "ALTER TABLE _hpplite_pending ADD COLUMN mode INTEGER NOT NULL DEFAULT 0");
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
    if (!has) rc = internalExec(pCtx, db, "ALTER TABLE _hpplite_meta ADD COLUMN rollup TEXT NOT NULL DEFAULT ''");
  }
  if (rc != SQLITE_OK) {
    sqlite3_free(pCtx);
    return rc;
  }
  loadMeta(pCtx);
  pCtx->seenSchema = schemaVersion(pCtx);
  pCtx->bClockPin = hpplite_clock_on(db);
  pragmaModeRead(pCtx);

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
  freeFunctionChecks(pCtx);
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
    int rc = internalExec(pCtx, pCtx->bkDb, bWrite ? "BEGIN IMMEDIATE" : "BEGIN");
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
    rc = internalExec(pCtx, pCtx->bkDb, "COMMIT");
  }
  if (bLook && rc == SQLITE_OK) {
    /* the baseline: after our own commit (which moves the version), with
    ** the application's commits since the last look consumed - one that
    ** lands after our snapshot leaves log rows, and is attributed by them */
    sqlite3_file_control(pCtx->bkDb, "main", SQLITE_FCNTL_DATA_VERSION, &pCtx->bkDataVersion);
    pCtx->bBkDataVersionValid = 1;
  }
  if (rc != SQLITE_OK) {
    internalExec(pCtx, pCtx->bkDb, "ROLLBACK");
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
