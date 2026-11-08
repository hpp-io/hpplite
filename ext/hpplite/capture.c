/*
** HPPLite - the authorizer on the application connection (#148).
**
** Page capture reads the commits from the WAL (pagemode.c, walread.c), so
** no statement is logged and nothing is refused for replay's sake. What the
** authorizer keeps: HPPLite's own names are not the application's to touch,
** ATTACH is refused (another file is not captured; VACUUM's unnamed temporary
** database is let through), the file stays in WAL and wal_autocheckpoint is
** HPPLite's (its hook reads the WAL). A refusal is said on stderr and kept
** in hpplite_status.last_refusal.
*/

#include "hpplite_int.h"
#include <string.h>
#include <stdio.h>

static int isHppliteTable(const char *zName) {
  return zName && sqlite3_strnicmp(zName, "_hpplite_", 9) == 0;
}

static int hppliteAuthorizerInner(HppliteCtx *pCtx, int op, const char *z1, const char *z2) {
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
      /* VACUUM attaches an unnamed temporary database to copy through; the
      ** copy lands in the file as pages like any other write (#148) */
      if (!z1 || !z1[0]) return SQLITE_OK;
      pCtx->zDenyWhy = "ATTACH: writes to another file are not captured";
      pCtx->nAuthDenied++;
      return SQLITE_DENY;
    case SQLITE_PRAGMA:
      /* page capture reads the WAL from this connection's wal hook, which
      ** PRAGMA wal_autocheckpoint would replace with SQLite's (#155); HPPLite
      ** checkpoints the file itself */
      if (z1 && z2 && sqlite3_stricmp(z1, "wal_autocheckpoint") == 0) {
        pCtx->zDenyWhy = "page capture: wal_autocheckpoint is HPPLite's (its hook reads the WAL; the file is checkpointed by HPPLite)";
        pCtx->nAuthDenied++;
        return SQLITE_DENY;
      }
      /* the file stays in WAL: the frames are what is captured */
      if (z1 && z2 && sqlite3_stricmp(z1, "journal_mode") == 0 && sqlite3_stricmp(z2, "wal") != 0
          && pCtx->bkDb != pCtx->db) {
        pCtx->zDenyWhy = "HPPLite keeps the database in WAL mode (journal_mode cannot be changed)";
        pCtx->nAuthDenied++;
        return SQLITE_DENY;
      }
      return SQLITE_OK;
    default:
      return SQLITE_OK;
  }
}

int hppliteAuthorizer(void *pArg, int op, const char *z1, const char *z2, const char *z3, const char *z4) {
  HppliteCtx *pCtx = (HppliteCtx*)pArg;
  int rc;
  (void)z3; (void)z4;
  if (!pCtx) return SQLITE_OK;
  if (pCtx->bInternal) return SQLITE_OK;
  /* VACUUM copies the database through one it attaches itself (its own
  ** statements, tables named as ours included): page mode's trace marks it */
  if (pCtx->pVacuumStmt) return SQLITE_OK;
  rc = hppliteAuthorizerInner(pCtx, op, z1, z2);
  if (rc == SQLITE_DENY) {
    fprintf(stderr, "hpplite: %s; statement refused (SQLITE_AUTH)\n", pCtx->zDenyWhy ? pCtx->zDenyWhy : "not authorized");
    snprintf(pCtx->zLastRefusal, sizeof(pCtx->zLastRefusal), "%s%s%s (the driver reports \"not authorized\")",
             pCtx->zDenyWhy ? pCtx->zDenyWhy : "not authorized", z1 ? ": " : "", z1 ? z1 : "");
  }
  return rc;
}
