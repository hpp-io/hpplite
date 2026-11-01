/*
** HPPLite - internal interface between the extension's source files.
**
** hpplite.h is what applications and node.c see; this header is what the
** files of the capture engine share with each other: the context (hpplite.h),
** and the functions one file defines for another. Everything declared here
** is internal: the names may change with the files.
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

/* hpplite.c: run SQL of HPPLite's own on the application connection, hooks silent */
int hppliteInternalExec(HppliteCtx *pCtx, sqlite3 *db, const char *zSql);

/* capture.c: PRAGMA schema_version (cached statement) */
int hppliteSchemaVersion(HppliteCtx *pCtx);

/* capture.c: read the connection pragmas the mode bits mirror (#28) */
void hpplitePragmaModeRead(HppliteCtx *pCtx);

/* capture.c: drop the function verdicts and the replica's function list */
void hppliteFreeFunctionChecks(HppliteCtx *pCtx);

/* capture.c: the hooks hpplite_init installs and rearmHooks re-installs */
int hppliteCommitHook(void *pArg);

/* capture.c: the hooks hpplite_init installs and rearmHooks re-installs */
void hppliteRollbackHook(void *pArg);

/* capture.c: the hooks hpplite_init installs and rearmHooks re-installs */
int hppliteTraceCallback(unsigned mask, void *pArg, void *pStmt, void *pX);

/* capture.c: the hooks hpplite_init installs and rearmHooks re-installs */
int hppliteAuthorizer(void *pArg, int op, const char *z1, const char *z2, const char *z3, const char *z4);

/* bookkeeping.c: a bookkeeping transaction, read or write; bkEnd commits or rolls back and notices foreign writes */
int hppliteBkBegin(HppliteCtx *pCtx, int bWrite);

int hppliteBkEnd(HppliteCtx *pCtx, int rc);

/* hpplite.c: bInternal around HPPLite's own statements when bookkeeping shares the application connection (:memory:) */
int hppliteBkEnter(HppliteCtx *pCtx);

void hppliteBkLeave(HppliteCtx *pCtx, int saved);

/* hpplite.c: load the _hpplite_meta row into the context's mirror */
int hppliteLoadMeta(HppliteCtx *pCtx);

/* finalize.c: advance _hpplite_meta past a batch (finalize, adopt, replay, skip) */
int hppliteAdvanceMeta(HppliteCtx *pCtx, const HppliteBatch *pBatch, sqlite3_int64 maxId, int bWriter);

/* status.c: the close sentinel and the hpplite_status table on a registered connection */
void hppliteStatusRegister(sqlite3 *db);

/* hpplite.c: the registry of application connections and their nodes */
struct HppliteNode *hppliteFindNode(sqlite3 *db);

void hppliteRemoveNode(sqlite3 *db);

/* hpplite.c: the connection close, from the status table's xDisconnect */
void hppliteOnClose(sqlite3 *db);

#endif /* HPPLITE_INT_H */
