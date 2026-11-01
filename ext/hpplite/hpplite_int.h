/*
** HPPLite - internal interface between the extension's source files.
**
** hpplite.h is what applications and node.c see; this header is what the
** files of the capture engine share with each other: the context (hpplite.h)
** and the functions one file defines for another. Everything here is
** internal; the names may change with the files. By file:
**
**   hpplite.c      lifecycle (init, shutdown), the _hpplite_meta mirror, the
**                  connection registry and the open/close API, extension entry
**   capture.c      the hooks on the application connection: trace, authorizer,
**                  commit and rollback; text rules; the stale-rerun machinery
**   capture_rows.c row capture: the TEMP triggers and their records
**   bookkeeping.c  the bookkeeping transaction and the foreign-write check
**   finalize.c     batch building, the size cut, finalizing and posted batches
**   replay.c       applying batches from the chain; adopt, skip, checkpoint
**   state_hash.c   the content hash (state root) and the block hash
**   status.c       the hpplite_status virtual table and the close sentinel
**   sha256.c       SHA-256
*/
#ifndef HPPLITE_INT_H
#define HPPLITE_INT_H

#include "hpplite.h"
#include "sha256.h"
#include "capture_rows.h"
#include "clock.h"
#include "batch.h"

/* mock builds: a test makes the next n bookkeeping COMMITs fail as SQLITE_FULL (a full disk) */
#ifdef HPPLITE_TEST_HOOKS
#define TEST_FAIL_COMMIT(rc) do { if ((rc) == SQLITE_OK && hpplite_test_fail_commits > 0) { hpplite_test_fail_commits--; (rc) = SQLITE_FULL; } } while (0)
#else
#define TEST_FAIL_COMMIT(rc) ((void)0)
#endif

/* hpplite.c */
int hppliteInternalExec(HppliteCtx *pCtx, sqlite3 *db, const char *zSql);  /* SQL of HPPLite's own on the application connection, hooks silent */
int hppliteBkEnter(HppliteCtx *pCtx);          /* bInternal around HPPLite's own statements when bookkeeping shares the */
void hppliteBkLeave(HppliteCtx *pCtx, int saved);  /* ...application connection (:memory:) */
int hppliteLoadMeta(HppliteCtx *pCtx);         /* the _hpplite_meta row into the context's mirror */
struct HppliteNode *hppliteFindNode(sqlite3 *db);  /* the registry of application connections and their nodes */
void hppliteRemoveNode(sqlite3 *db);
void hppliteOnClose(sqlite3 *db);              /* the connection close, from the status table's xDisconnect */

/* capture.c */
int hppliteTraceCallback(unsigned mask, void *pArg, void *pStmt, void *pX);  /* the hooks hpplite_init installs */
int hppliteAuthorizer(void *pArg, int op, const char *z1, const char *z2, const char *z3, const char *z4);  /* ...and re-arms */
int hppliteCommitHook(void *pArg);
void hppliteRollbackHook(void *pArg);
int hppliteSchemaVersion(HppliteCtx *pCtx);    /* PRAGMA schema_version (cached statement) */
void hpplitePragmaModeRead(HppliteCtx *pCtx);  /* the connection pragmas the mode bits mirror (#28) */
void hppliteFreeFunctionChecks(HppliteCtx *pCtx);  /* the function verdicts and the replica's function list */

/* bookkeeping.c */
int hppliteBkBegin(HppliteCtx *pCtx, int bWrite);  /* a bookkeeping transaction, read or write... */
int hppliteBkEnd(HppliteCtx *pCtx, int rc);    /* ...committed or rolled back; a foreign write is noticed here (#37) */

/* finalize.c */
int hppliteAdvanceMeta(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId, int bWriter);  /* past a batch: finalize, adopt, replay, skip */

/* status.c */
void hppliteStatusRegister(sqlite3 *db);       /* the close sentinel and the hpplite_status table on a registered connection */

#endif /* HPPLITE_INT_H */
