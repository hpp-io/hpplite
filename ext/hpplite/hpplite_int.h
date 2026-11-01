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

#endif /* HPPLITE_INT_H */
