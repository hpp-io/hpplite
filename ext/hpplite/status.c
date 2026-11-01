/*
** HPPLite - the hpplite_status virtual table.
**
** An eponymous virtual table over the node's counters, _hpplite_meta and the
** lease, read on the application thread (which is also where the hooks are
** re-armed if an application replaced them).
*/

#include "hpplite_int.h"
#include "config.h"
#include "crypto.h"
#include "l1_interface.h"
#include "node.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

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
    hppliteOnClose(p->db);
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
    HppliteNode *node = hppliteFindNode(db);
    if (!node) return;
    hppliteRemoveNode(db);
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
        HppliteNode *n = hppliteFindNode(((StatusVtab *)pCur->pVtab)->db);
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
    HppliteNode *node = hppliteFindNode(p->db);
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

/* The status surface of a registered connection: the close sentinel and the
** table, touched once so its instance lives until the connection closes */
void hppliteStatusRegister(sqlite3 *db) {
    sqlite3_create_function_v2(db, "_hpplite_close_sentinel", 0, SQLITE_UTF8, db,
                               closeSentinelFunc, NULL, NULL, closeSentinelDestroy);
    if (sqlite3_create_module(db, "hpplite_status", &hpplite_status_module, NULL) == SQLITE_OK) {
        sqlite3_exec(db, "SELECT * FROM hpplite_status LIMIT 0", NULL, NULL, NULL);
    }
}
