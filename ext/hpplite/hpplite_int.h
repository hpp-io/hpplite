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
**   capture.c      the authorizer on the application connection (#148)
**   pagemode.c     page capture (#146): the commits' pages from the WAL, the page batches
**   pages.c        the page root and the page batch format; walread.c reads the WAL
**   bookkeeping.c  the bookkeeping transaction and the foreign-write check
**   finalize.c     the meta row past a posted page batch, the posted copies, the chain hash,
**                  the verifier's root ring, a foreign height skipped, the checkpoint
**   state_hash.c   the content hash of a database (hpplite-verify --db)
**   status.c       the hpplite_status virtual table and the close sentinel
**   sha256.c       SHA-256
*/
#ifndef HPPLITE_INT_H
#define HPPLITE_INT_H

#include "hpplite.h"
#include "sha256.h"
#include "batch.h"

/* mock builds: a test makes the next n bookkeeping COMMITs fail as SQLITE_FULL (a full disk) */
#ifdef HPPLITE_TEST_HOOKS
#define TEST_FAIL_COMMIT(rc) do { if ((rc) == SQLITE_OK && hpplite_test_fail_commits > 0) { hpplite_test_fail_commits--; (rc) = SQLITE_FULL; } } while (0)
#else
#define TEST_FAIL_COMMIT(rc) ((void)0)
#endif

/* HPPLite's own statements on the application connection: the hooks ignore
** them while bInternal is set. Scoped: what hppliteInternalBegin() returns
** goes to hppliteInternalEnd() on every way out of the scope, so no path
** leaves the flag set (which silently drops captures, #20) or clears one a
** caller holds. */
static inline int hppliteInternalBegin(HppliteCtx *pCtx) { int saved = pCtx->bInternal; pCtx->bInternal = 1; return saved; }
static inline void hppliteInternalEnd(HppliteCtx *pCtx, int saved) { pCtx->bInternal = saved; }

/* hpplite.c */
int hppliteInternalExec(HppliteCtx *pCtx, sqlite3 *db, const char *zSql);  /* SQL of HPPLite's own on the application connection, hooks silent */
int hppliteBkEnter(HppliteCtx *pCtx);          /* bInternal around HPPLite's own statements when bookkeeping shares the */
void hppliteBkLeave(HppliteCtx *pCtx, int saved);  /* ...application connection (:memory:) */
int hppliteLoadMeta(HppliteCtx *pCtx);         /* the _hpplite_meta row into the context's mirror */
struct HppliteNode *hppliteFindNode(sqlite3 *db);  /* the registry of application connections and their nodes */
void hppliteRemoveNode(sqlite3 *db);
void hppliteOnClose(sqlite3 *db);              /* the connection close, from the status table's xDisconnect */

/* capture.c */
int hppliteAuthorizer(void *pArg, int op, const char *z1, const char *z2, const char *z3, const char *z4);  /* ...and re-arms */

/* bookkeeping.c */
int hppliteBkBegin(HppliteCtx *pCtx, int bWrite);  /* a bookkeeping transaction, read or write... */
int hppliteBkEnd(HppliteCtx *pCtx, int rc);    /* ...committed or rolled back; a foreign write is noticed here (#37) */

/* finalize.c */

/* status.c */
void hppliteStatusRegister(sqlite3 *db);       /* the close sentinel and the hpplite_status table on a registered connection */

#endif /* HPPLITE_INT_H */
