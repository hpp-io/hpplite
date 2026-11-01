/*
** hpplite-verify: rebuild a database from the chain alone and check it.
**
**   HPPLITE_PARAMS="l1=hpp-sepolia&keyfile=/path/to/key" \
**     hpplite-verify [--db /path/to/live.db] [--keep <dir>]
**
** Opens an empty file with role=verify and the same parameters the
** application uses (l1=/rpc=/factory=/contract=, the key, dbname=): every
** batch is fetched, opened and replayed, and after each batch that carries
** a state root the content hash of the rebuilt database is compared with
** the posted one. The root anchored on the contract by the last checkpoint
** is compared with the rebuilt content at that height. A verifier never
** posts, never claims the lease and never creates a rollup (#5).
**
** --db <file>: also compare the live database with the rebuilt one - the
** same content hash as the state root; when they differ and nothing is
** pending in the live file, the tables that differ are named.
**
** Exit 0: verified (and the live file equals the chain, with --db);
** 1: a root mismatch, a sync that stopped short, or a live file that
** differs; 2: cannot run (parameters, chain, no rollup).
*/

#include "hpplite.h"
#include "node.h"
#include "l1_interface.h"
#include "crypto.h"
#include "config.h"
#include "store.h"
#include <time.h>
#include <ftw.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *ftwbuf) {
    (void)sb; (void)type; (void)ftwbuf;
    return remove(path);
}

static char *status_text(sqlite3 *db, const char *col) {
    static char buf[8][512];
    static int which;
    char *out = buf[which = (which + 1) % 8];
    char sql[96];
    sqlite3_stmt *st;
    snprintf(sql, sizeof(sql), "SELECT %s FROM hpplite_status", col);
    out[0] = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return out;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        snprintf(out, 512, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void hex(const unsigned char *h, char *out) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = d[h[i] >> 4]; out[2 * i + 1] = d[h[i] & 15]; }
    out[64] = 0;
}

/* Content hash of one table: every row in rowid (or key) order, typed */
static int table_hash(sqlite3 *db, const char *zTab, unsigned char out[32], sqlite3_int64 *pRows) {
    char *zSql = sqlite3_mprintf("SELECT * FROM main.\"%w\" ORDER BY rowid", zTab);
    sqlite3_stmt *st = NULL;
    size_t cap = 1 << 16, len = 0;
    unsigned char *buf = malloc(cap);
    int rc;
    *pRows = 0;
    if (!zSql || !buf) { sqlite3_free(zSql); free(buf); return SQLITE_NOMEM; }
    rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
    if (rc != SQLITE_OK) {                                   /* WITHOUT ROWID: order by every column */
        sqlite3_free(zSql);
        zSql = sqlite3_mprintf("SELECT * FROM main.\"%w\" ORDER BY 1", zTab);
        rc = zSql ? sqlite3_prepare_v2(db, zSql, -1, &st, NULL) : SQLITE_NOMEM;
    }
    sqlite3_free(zSql);
    if (rc != SQLITE_OK) { free(buf); return rc; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        int n = sqlite3_column_count(st);
        (*pRows)++;
        for (int i = 0; i < n; i++) {
            int t = sqlite3_column_type(st, i);
            const unsigned char *z = t == SQLITE_NULL ? (const unsigned char *)"" : sqlite3_column_blob(st, i);
            int nz = t == SQLITE_NULL ? 0 : sqlite3_column_bytes(st, i);
            while (len + (size_t)nz + 6 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) { sqlite3_finalize(st); return SQLITE_NOMEM; } }
            buf[len++] = (unsigned char)t;
            buf[len++] = (unsigned char)(nz >> 24); buf[len++] = (unsigned char)(nz >> 16);
            buf[len++] = (unsigned char)(nz >> 8);  buf[len++] = (unsigned char)nz;
            if (nz) memcpy(buf + len, z, (size_t)nz);
            len += (size_t)nz;
        }
        buf[len++] = 0xff;
    }
    sqlite3_finalize(st);
    hpplite_keccak256(buf, len, out);
    free(buf);
    return SQLITE_OK;
}

/* The tables that differ between two databases: named on stdout, counted */
static int report_table_differences(sqlite3 *a, sqlite3 *b) {
    sqlite3_stmt *st;
    int nDiff = 0;
    const char *zList = "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\'"
                        " AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\' ORDER BY name";
    if (sqlite3_prepare_v2(a, zList, -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *zTab = (const char *)sqlite3_column_text(st, 0);
        unsigned char ha[32], hb[32];
        sqlite3_int64 ra = 0, rb = 0;
        char *q = sqlite3_mprintf("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = %Q", zTab);
        int inB = q && scalar(b, q) == 1;
        sqlite3_free(q);
        if (!inB) { printf("  table %s: only in the live database\n", zTab); nDiff++; continue; }
        if (table_hash(a, zTab, ha, &ra) != SQLITE_OK || table_hash(b, zTab, hb, &rb) != SQLITE_OK) continue;
        if (memcmp(ha, hb, 32) != 0) {
            printf("  table %s: differs (%lld rows live, %lld rebuilt)\n", zTab, (long long)ra, (long long)rb);
            nDiff++;
        }
    }
    sqlite3_finalize(st);
    /* tables the chain has and the live file does not */
    if (sqlite3_prepare_v2(b, zList, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *zTab = (const char *)sqlite3_column_text(st, 0);
            char *q = sqlite3_mprintf("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = %Q", zTab);
            if (q && scalar(a, q) != 1) { printf("  table %s: only in the rebuilt database\n", zTab); nDiff++; }
            sqlite3_free(q);
        }
        sqlite3_finalize(st);
    }
    return nDiff;
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: HPPLITE_PARAMS=\"l1=...&keyfile=...\" %s [--db <live database>] [--keep <dir>]\n", argv0);
    return 2;
}

int main(int argc, char **argv) {
    const char *zLive = NULL, *zKeep = NULL;
    char dir[1024];
    int rc = 0, i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) zLive = argv[++i];
        else if (strcmp(argv[i], "--keep") == 0 && i + 1 < argc) zKeep = argv[++i];
        else return usage(argv[0]);
    }
    const char *params = getenv("HPPLITE_PARAMS");
    if (!params || !params[0]) {
        fprintf(stderr, "hpplite-verify: HPPLITE_PARAMS must name the chain and the key (l1= or rpc=/factory=/contract=, keyfile=/privkey=)\n");
        return 2;
    }
    if (zKeep) {
        snprintf(dir, sizeof dir, "%s", zKeep);
        (void)!mkdir(dir, 0700);
    } else {
        const char *tmp = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/hpplite-verify-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
        if (!mkdtemp(dir)) { perror("hpplite-verify: mkdtemp"); return 2; }
    }

    /* the verifier's own datadir, first so it wins over one in the parameters */
    char *uri = sqlite3_mprintf("file:%s/rebuilt.db?hpplite=on&role=verify&interval=0&datadir=%s&%s", dir, dir, params);
    sqlite3 *db = uri ? hpplite_open(uri) : NULL;
    sqlite3_free(uri);
    if (!db) {
        fprintf(stderr, "hpplite-verify: cannot open with these parameters (%s)\n", hpplite_config_error() ? hpplite_config_error() : "see above");
        rc = 2;
        goto done;
    }
    HppliteNode *node = hpplite_get_node(db);
    if (!node || !node->l1) {
        fprintf(stderr, "hpplite-verify: no chain connection (%s)\n", status_text(db, "last_error"));
        rc = 2;
        goto close;
    }

    /* A long rebuild races the sequencer: batches posted meanwhile are
    ** replayed too, until the chain and the rebuild agree or a sync stops */
    uint64_t onChain = 0, total = 0, rebuilt = 0;
    for (i = 0; i < 100; i++) {
        sqlite3_int64 height;
        hpplite_l1_get_da_state(node->l1, &onChain, &total, NULL);
        height = scalar(db, "SELECT height FROM hpplite_status");   /* next batch number */
        rebuilt = height > 0 ? (uint64_t)height - 1 : 0;
        if (rebuilt >= onChain || node->state == HPPLITE_STATE_ERROR) break;
        if (hpplite_node_sync_from_l1(node, 1) <= 0) break;
    }
    const char *zState = status_text(db, "state");
    const char *zErr = status_text(db, "last_error");
    sqlite3_int64 compared = scalar(db, "SELECT roots_compared FROM hpplite_status");
    sqlite3_int64 mismatches = scalar(db, "SELECT root_mismatches FROM hpplite_status");
    sqlite3_int64 skipped = scalar(db, "SELECT replay_skipped FROM hpplite_status");
    sqlite3_int64 dropped = scalar(db, "SELECT txns_dropped FROM hpplite_status");
    char contract[43] = "", da[43] = "";
    hpplite_l1_get_addresses(node->l1, contract, da);

    printf("rollup %s: %llu batches on chain, %llu replayed", contract, (unsigned long long)onChain, (unsigned long long)rebuilt);
    if (rebuilt < onChain) {
        printf("\nhpplite-verify: sync stopped at batch %llu: %s\n", (unsigned long long)rebuilt + 1, zErr[0] ? zErr : zState);
        rc = 1;
    } else {
        printf("\n");
    }
    printf("posted roots compared: %lld, mismatches: %lld", (long long)compared, (long long)mismatches);
    if (mismatches > 0) { printf(" <- the content does not match what was posted"); rc = 1; }
    printf("\n");
    if (skipped > 0 || dropped > 0) {
        printf("statements skipped on replay: %lld, transactions dropped: %lld (replica differs from the writer)\n",
               (long long)skipped, (long long)dropped);
        rc = 1;
    }

    /* the root the contract anchors, against the rebuilt content at that height */
    {
        HppliteL1State *st = hpplite_l1_get_state(node->l1);
        if (st && st->lastCheckpointHeight > 0) {
            char want[65], got[65];
            hex(st->lastStateRoot, want);
            if (node->ctx->rootCmpHeight == st->lastCheckpointHeight) {
                hex(node->ctx->rootCmp, got);
                if (memcmp(st->lastStateRoot, node->ctx->rootCmp, 32) == 0) {
                    printf("on-chain checkpoint at batch %llu: root %s matches the rebuilt content\n",
                           (unsigned long long)st->lastCheckpointHeight, want);
                } else {
                    printf("on-chain checkpoint at batch %llu: root %s, rebuilt content %s <- MISMATCH\n",
                           (unsigned long long)st->lastCheckpointHeight, want, got);
                    rc = 1;
                }
            } else {
                printf("on-chain checkpoint at batch %llu (root %s): not reached by the rebuild (last compared: batch %llu)\n",
                       (unsigned long long)st->lastCheckpointHeight, want, (unsigned long long)node->ctx->rootCmpHeight);
                if (rebuilt >= st->lastCheckpointHeight) rc = 1;
            }
        } else if (st) {
            printf("no checkpoint on chain yet (checkpoint interval %llu): posted roots only\n", (unsigned long long)st->checkpointInterval);
        }
        hpplite_l1_state_free(st);
    }
    /* the contract's rolling hash over every batch (#33) against the one the rebuild computed */
    {
        uint64_t h = 0;
        uint8_t chain[32];
        if (hpplite_l1_get_chain_hash(node->l1, &h, chain) == 0) {
            const char *mine = status_text(db, "chain_hash");
            char want[65];
            hex(chain, want);
            if (mine[0] == '0' && (mine[1] == 'x' || mine[1] == 'X')) mine += 2;
            if (rebuilt < h) {
                printf("chain hash: the contract's covers %llu batches, the rebuild %llu: not compared\n", (unsigned long long)h, (unsigned long long)rebuilt);
            } else if (strcasecmp(mine, want) == 0) {
                printf("chain hash over %llu batches: matches the contract's\n", (unsigned long long)h);
            } else {
                printf("chain hash over %llu batches: contract %s, rebuilt %s <- MISMATCH\n", (unsigned long long)h, want, mine[0] ? mine : "(none)");
                rc = 1;
            }
        } else {
            printf("no rolling hash on this contract (DA v1): posted roots and the checkpoint only\n");
        }
    }
    /* where the last batch's bytes are: on the chain, or in a store under a provider's receipt (#11) */
    if (onChain > 0) {
        uint8_t h[32], *receipt = NULL;
        char *zRef = NULL;
        size_t nReceipt = 0;
        if (hpplite_l1_get_commitment(node->l1, onChain, h, &zRef, &receipt, &nReceipt) == 0) {
            unsigned char signer[20];
            uint64_t expires = 0;
            printf("batch %llu: bytes in a store, reference %s", (unsigned long long)onChain, zRef);
            if (hpplite_store_receipt_decode(receipt, nReceipt, signer, &expires, NULL) == 0) {
                char zs[41];
                time_t t = (time_t)expires;
                char when[40];
                hpplite_bytes_to_hex(signer, 20, zs);
                strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
                printf("; receipt signed by 0x%s, held until %s%s", zs, when, expires > (uint64_t)time(NULL) ? "" : " <- EXPIRED");
            } else {
                printf("; no receipt");
            }
            printf("\n");
            sqlite3_free(zRef);
            sqlite3_free(receipt);
        }
    }
    {
        unsigned char root[32]; char z[65];
        if (hpplite_db_hash(db, root) == SQLITE_OK) { hex(root, z); printf("rebuilt state root: 0x%s\n", z); }
    }

    if (zLive) {
        sqlite3 *live = NULL;
        if (sqlite3_open_v2(zLive, &live, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            fprintf(stderr, "hpplite-verify: cannot open %s: %s\n", zLive, live ? sqlite3_errmsg(live) : "?");
            rc = 2;
        } else {
            unsigned char a[32], b[32];
            sqlite3_int64 pending = scalar(live, "SELECT count(*) FROM _hpplite_pending");
            if (hpplite_db_hash(live, a) == SQLITE_OK && hpplite_db_hash(db, b) == SQLITE_OK) {
                if (memcmp(a, b, 32) == 0) {
                    printf("live database %s: equals the chain\n", zLive);
                } else {
                    printf("live database %s: DIFFERS from the chain", zLive);
                    if (pending > 0) printf(" (%lld rows pending, not posted yet: flush, then verify again)", (long long)pending);
                    printf("\n");
                    int n = report_table_differences(live, db);
                    if (n == 0) printf("  (no table differs row for row: the schema text differs)\n");
                    rc = rc ? rc : 1;
                }
            } else {
                fprintf(stderr, "hpplite-verify: cannot hash %s\n", zLive);
                rc = 2;
            }
        }
        sqlite3_close(live);
    }
    printf("%s\n", rc == 0 ? "verified" : rc == 1 ? "NOT verified" : "could not verify");

close:
    sqlite3_close(db);
done:
    if (!zKeep) nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    else printf("rebuilt database kept in %s\n", dir);
    return rc;
}
