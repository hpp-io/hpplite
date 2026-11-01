/*
** HPPLite - the maintained per-table Merkle trees of the version 2 state
** root (#9). The trees live in _hpplite_tree and reflect the file up to
** _hpplite_meta.tree_seq of the pending log; changes since are taken in
** from the log (row records name their keys) into an in-memory overlay,
** which a bookkeeping write transaction flushes. A statement entry (DDL,
** VACUUM, text capture) marks every tree stale: rebuilt from a scan at the
** next root. hpplite_db_hash() on a plain file builds the same trees from
** a scan and needs none of this.
*/
#ifndef HPPLITE_STATE_TREE_H
#define HPPLITE_STATE_TREE_H
#include "hpplite_sqlite.h"
struct HppliteCtx;
struct HppliteBatch;
/* The tables and columns, on a file that may be from before */
int hppliteTreeSchema(struct HppliteCtx *pCtx, sqlite3 *db);
/* Take in the pending log since the trees' seq (inside a bookkeeping transaction, or on the shared connection) */
int hppliteTreeSync(struct HppliteCtx *pCtx);
/* The version 2 root as the file is now (syncs first) */
int hppliteTreeRoot(struct HppliteCtx *pCtx, unsigned char out[32]);
/* Inside a bookkeeping write transaction: the overlay to _hpplite_tree, stale trees rebuilt, the seq advanced */
int hppliteTreeFlush(struct HppliteCtx *pCtx);
/* A replica, inside the write transaction after a batch was applied: the
** entries it ran (carried ones included), each by xEntry(pArg, i) */
int hppliteTreeTouchEntries(struct HppliteCtx *pCtx, int n,
                            void (*xEntry)(void *pArg, int i, const char **pzSql, int *pMode), void *pArg);
void hppliteTreeFree(struct HppliteCtx *pCtx);
#endif
