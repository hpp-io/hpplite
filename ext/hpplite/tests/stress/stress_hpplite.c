/*
** HPPLite stress test (mock L1, no network)
**
** Drives a database to a target size through HPPLite with everything on:
** encryption, the DA size limit (so batches are cut into series), a short
** checkpoint interval (so roots are computed and checkpoints submitted
** repeatedly), then rebuilds a fresh database from the mock L1 and compares
** content roots. Reports timings. Exit code 1 on any mismatch.
**
** Usage: stress_hpplite [--mb N] [--checkpoint N] [--limit BYTES] [--rows-per-txn N]
** Defaults: 100 MB, checkpoint every 20 batches, 128 KB limit, 500 rows/txn.
** Not part of ctest; `make stress`.
*/

#include "node.h"
#include "cipher.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DIR "/tmp/hpplite_stress"

static const unsigned char PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};

static double now_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static sqlite3_int64 db_bytes(sqlite3 *db) {
    sqlite3_stmt *s; sqlite3_int64 v = 0;
    sqlite3_exec(db, "PRAGMA wal_checkpoint(PASSIVE)", NULL, NULL, NULL);
    sqlite3_prepare_v2(db, "SELECT page_count * page_size FROM pragma_page_count, pragma_page_size", -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static sqlite3_int64 count_rows(sqlite3 *db) {
    sqlite3_stmt *s; sqlite3_int64 v = -1;
    sqlite3_prepare_v2(db, "SELECT count(*) FROM t", -1, &s, NULL);
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static HppliteNode *open_node(const char *path, HppliteL1 *l1, int checkpointInterval) {
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "stress";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = (char *)path;
    config.checkpointInterval = checkpointInterval;
    HppliteNode *n = hpplite_node_create(&config);
    if (n) { hpplite_node_set_l1(n, l1); hpplite_node_sync_from_l1(n, 1); hpplite_node_start(n); }
    return n;
}

int main(int argc, char **argv) {
    int mb = 100, checkpointEvery = 20, rowsPerTxn = 500;
    size_t limit = 128 * 1024;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--checkpoint") && i + 1 < argc) checkpointEvery = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--limit") && i + 1 < argc) limit = (size_t)atol(argv[++i]);
        else if (!strcmp(argv[i], "--rows-per-txn") && i + 1 < argc) rowsPerTxn = atoi(argv[++i]);
        else { fprintf(stderr, "usage: %s [--mb N] [--checkpoint N] [--limit BYTES] [--rows-per-txn N]\n", argv[0]); return 2; }
    }
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== HPPLite stress: %d MB, checkpoint every %d batches, DA limit %zu, %d rows/txn ===\n",
           mb, checkpointEvery, limit, rowsPerTxn);
    (void)!system("rm -rf " DIR " && mkdir -p " DIR);

    HppliteL1 *l1 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l1, limit);
    HppliteNode *w = open_node(DIR "/writer.db", l1, checkpointEvery);
    if (!w) { printf("open failed\n"); return 1; }
    if (!w->cipher) { printf("expected encryption on\n"); return 1; }
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, name TEXT, score REAL, payload BLOB)", NULL);

    /* ---- write phase: ~100 bytes per row, flush every 10 txns like a timer would ---- */
    double t0 = now_ms(), tWrite = 0, tFlush = 0;
    sqlite3_int64 target = (sqlite3_int64)mb * 1024 * 1024, rows = 0, lastRoot = 0;
    int txns = 0, batchesBefore;
    char sql[256];
    while (db_bytes(w->db) < target) {
        double a = now_ms();
        sqlite3_exec(w->db, "BEGIN", NULL, NULL, NULL);
        for (int i = 0; i < rowsPerTxn; i++) {
            rows++;
            /* deterministic payload: replayed SQL must produce identical rows
            ** (randomblob() here would make the history unreproducible) */
            snprintf(sql, sizeof(sql),
                "INSERT INTO t VALUES(%lld,'user-%08lld@example.com',%lld.25,x'%016llx%016llx')",
                (long long)rows, (long long)rows, (long long)(rows % 1000),
                (unsigned long long)(rows * 0x9E3779B97F4A7C15ULL), (unsigned long long)(rows ^ 0xDEADBEEFCAFEBABEULL));
            sqlite3_exec(w->db, sql, NULL, NULL, NULL);
        }
        sqlite3_exec(w->db, "COMMIT", NULL, NULL, NULL);
        tWrite += now_ms() - a;
        if (++txns % 10 == 0) {
            a = now_ms();
            batchesBefore = (int)hpplite_node_get_height(w);
            hpplite_node_flush_batch(w);
            tFlush += now_ms() - a;
            if ((int)hpplite_node_get_height(w) - batchesBefore > 1) lastRoot = rows;
        }
        if (txns % 200 == 0)
            printf("  %6lld MB  %9lld rows  height %6llu  checkpoints %d  (write %.1fs, flush %.1fs)\n",
                   (long long)(db_bytes(w->db) >> 20), (long long)rows,
                   (unsigned long long)hpplite_node_get_height(w),
                   hpplite_l1_mock_get_checkpoint_count(l1), tWrite / 1000, tFlush / 1000);
    }
    double a = now_ms();
    hpplite_node_flush_batch(w);
    tFlush += now_ms() - a;
    double tTotal = now_ms() - t0;

    uint64_t height = hpplite_node_get_height(w);
    int checkpoints = hpplite_l1_mock_get_checkpoint_count(l1);
    sqlite3_int64 bytes = db_bytes(w->db);
    unsigned char rootW[32];
    a = now_ms();
    hpplite_node_get_state_root(w, rootW);
    double tRoot = now_ms() - a;

    /* ---- checks on the writer side ---- */
    int ok = 1;
    if (hpplite_node_pending_count(w) != 0) { printf("FAIL: pending not empty\n"); ok = 0; }
    if (count_rows(w->db) != rows) { printf("FAIL: writer has %lld rows, wrote %lld\n", (long long)count_rows(w->db), (long long)rows); ok = 0; }
    if (checkpoints < 1) { printf("FAIL: no checkpoint submitted\n"); ok = 0; }
    const HppliteCheckpoint *cp = hpplite_l1_mock_get_last_checkpoint(l1);
    /* the last checkpoint covers the state up to its toHeight; if nothing was
    ** written after it, it must equal the live root */
    int cpIsCurrent = cp && cp->toHeight == height - 1;
    if (cpIsCurrent && memcmp(cp->postStateRoot, rootW, 32) != 0) { printf("FAIL: last checkpoint root != content root\n"); ok = 0; }
    /* every L1 batch must fit the limit and be sealed; series roots only on closing batches */
    size_t maxLen = 0; int sealed = 0, withRoot = 0, partial = 0;
    for (uint64_t h = 1; h < height; h++) {
        size_t len = 0; uint8_t *raw = hpplite_l1_get_batch(l1, h, &len);
        if (!raw) { printf("FAIL: batch %llu missing\n", (unsigned long long)h); ok = 0; break; }
        if (len > maxLen) maxLen = len;
        if (len > limit) { printf("FAIL: batch %llu is %zu bytes > limit\n", (unsigned long long)h, len); ok = 0; }
        sealed += hpplite_envelope_is_sealed(raw, len);
        free(raw);
        HppliteBatch *b = hpplite_node_fetch_batch(w, h);
        if (!b) { printf("FAIL: batch %llu does not open\n", (unsigned long long)h); ok = 0; break; }
        withRoot += b->hasRoot; partial += b->partial;
        if (b->partial && b->hasRoot) { printf("FAIL: partial batch %llu carries a root\n", (unsigned long long)h); ok = 0; }
        hpplite_batch_free(b);
    }
    if (sealed != (int)(height - 1)) { printf("FAIL: %d of %llu batches sealed\n", sealed, (unsigned long long)(height - 1)); ok = 0; }
    hpplite_node_destroy(w);

    printf("\nwriter: %lld rows, %lld MB on disk, %llu batches (%d partial, %d with root, largest %zu B), %d checkpoints\n",
           (long long)rows, (long long)(bytes >> 20), (unsigned long long)(height - 1), partial, withRoot, maxLen, checkpoints);
    printf("        write %.1fs (%.0f rows/s)  flush %.1fs  total %.1fs  one root %.0f ms\n",
           tWrite / 1000, rows / (tWrite / 1000), tFlush / 1000, tTotal / 1000, tRoot);

    /* ---- reconstruct from L1 ---- */
    a = now_ms();
    HppliteNode *r = open_node(DIR "/reader.db", l1, checkpointEvery);
    double tReplay = now_ms() - a;
    if (!r) { printf("FAIL: reader open\n"); return 1; }
    unsigned char rootR[32];
    hpplite_node_get_state_root(r, rootR);
    sqlite3_int64 rowsR = count_rows(r->db);
    printf("reader: applied %llu batches in %.1fs (%.0f rows/s), %lld rows\n",
           (unsigned long long)(hpplite_node_get_height(r) - 1), tReplay / 1000, rowsR / (tReplay / 1000),
           (long long)rowsR);
    if (rowsR != rows) { printf("FAIL: row count %lld != %lld\n", (long long)rowsR, (long long)rows); ok = 0; }
    if (memcmp(rootW, rootR, 32) != 0) {
        printf("FAIL: content root differs after reconstruction\n"); ok = 0;
        /* say which rows differ: the writer file is still on disk */
        sqlite3_exec(r->db, "ATTACH '" DIR "/writer.db' AS w", NULL, NULL, NULL);
        static const char *aDiff[] = {
            "SELECT 'only in reader', id FROM t WHERE id NOT IN (SELECT id FROM w.t) LIMIT 5",
            "SELECT 'only in writer', id FROM w.t WHERE id NOT IN (SELECT id FROM t) LIMIT 5",
            "SELECT 'differs', t.id FROM t JOIN w.t AS wt ON wt.id = t.id"
            " WHERE t.name IS NOT wt.name OR t.score IS NOT wt.score OR t.payload IS NOT wt.payload LIMIT 5",
            NULL };
        for (int i = 0; aDiff[i]; i++) {
            sqlite3_stmt *st = NULL;
            if (sqlite3_prepare_v2(r->db, aDiff[i], -1, &st, NULL) == SQLITE_OK) {
                while (sqlite3_step(st) == SQLITE_ROW)
                    printf("  %s: id %lld\n", sqlite3_column_text(st, 0), (long long)sqlite3_column_int64(st, 1));
                sqlite3_finalize(st);
            }
        }
    }
    if (hpplite_node_get_height(r) != height) { printf("FAIL: height %llu != %llu\n", (unsigned long long)hpplite_node_get_height(r), (unsigned long long)height); ok = 0; }
    if (r->ctx->nReplaySkipped) { printf("FAIL: statements skipped on replay\n"); ok = 0; }
    r->l1 = NULL;
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    (void)!system("rm -rf " DIR);
    printf("\n=== %s ===\n", ok ? "STRESS PASSED" : "STRESS FAILED");
    return ok ? 0 : 1;
}
