/*
** The state root (#9): version 2 is a hash of per-table Merkle trees kept
** up to date from the log, so a checkpoint costs what changed, not the
** database; still a pure function of the file's content.
*/
#include "test_lib.h"
#include "smt.h"
#include "batch.h"
#include "cipher.h"

#define DIR "/tmp/hpplite_test_state_root"

static HppliteNode *open_writer(const char *db, HppliteL1 *l1, int checkpointInterval) {
    TestNodeOpts o = { .checkpointInterval = checkpointInterval };
    return test_open_node(db, l1, &o);
}

static void test_incremental(void) {
    TEST("one row changed in a 10,000-row table: the next root reads under 100 rows, not the table");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer(DIR "/w.db", l1, 1);          /* a root with every batch */
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT, c REAL)", NULL);
    hpplite_node_exec(w, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 10000)"
                         " INSERT INTO t SELECT i, 'row ' || i, i / 7.0 FROM s; COMMIT", NULL);
    if (hpplite_node_flush_batch(w) < 1) FAIL("flush 1");             /* 10,000 rows are several batches; the series ends with a root */
    long first = hpplite_test_rows_hashed;
    hpplite_test_rows_hashed = 0;
    hpplite_node_exec(w, "UPDATE t SET b = 'changed' WHERE a = 5000", NULL);
    if (hpplite_node_flush_batch(w) < 1) FAIL("flush 2");
    long second = hpplite_test_rows_hashed;
    printf("(rows read: first root %ld, after one change %ld) ", first, second);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (second >= 100) FAIL("the root after one changed row read the whole table");
    PASS();
}


static int hex_eq(const unsigned char *a, const unsigned char *b) { return memcmp(a, b, 32) == 0; }

/* the maintained root (hpplite_status.state_root) against the scan of the same file */
static int root_matches_scan(HppliteNode *n, const char *who) {
    unsigned char kept[32], scan[32];
    hpplite_node_get_state_root(n, kept);
    if (hpplite_db_hash(n->db, scan) != SQLITE_OK) { printf("(%s: scan failed) ", who); return 0; }
    if (!hex_eq(kept, scan)) { printf("(%s: maintained root != scan root) ", who); return 0; }
    return 1;
}

static void test_matches_scan(void) {
    TEST("the maintained root equals the scan root after inserts, updates, deletes, a key change, WITHOUT ROWID rows and DDL; writer and replica agree");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer(DIR "/w.db", l1, 1);
    if (!w) FAIL("open");
    const char *steps[] = {
        "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT); CREATE TABLE k(x TEXT, y INTEGER, PRIMARY KEY(y, x)) WITHOUT ROWID; CREATE TABLE r(v)",
        "INSERT INTO t VALUES(1,'one'),(2,'two'),(3,'three'); INSERT INTO k VALUES('a',1),('b',2); INSERT INTO r VALUES(1.5),(NULL),(x'00ff')",
        "UPDATE t SET b='TWO' WHERE a=2; DELETE FROM t WHERE a=3; UPDATE k SET y=9 WHERE x='a'; UPDATE r SET v='text' WHERE v IS NULL",
        "UPDATE t SET a=10 WHERE a=1; INSERT INTO t VALUES(4,'four'); DELETE FROM k WHERE x='b'",
        "ALTER TABLE t ADD COLUMN c DEFAULT 7; INSERT INTO t VALUES(5,'five',8)",
        "CREATE TABLE u(a); DROP TABLE r; INSERT INTO u VALUES(1)",
        "DELETE FROM t",
        NULL };
    for (int i = 0; steps[i]; i++) {
        if (hpplite_node_exec(w, steps[i], NULL) != SQLITE_OK) { printf("(step %d) ", i); FAIL("writer SQL"); }
        if (hpplite_node_flush_batch(w) < 1) { printf("(step %d) ", i); FAIL("flush"); }
        if (!root_matches_scan(w, "writer")) { printf("(step %d) ", i); FAIL("writer root"); }
    }
    HppliteNode *r = open_writer(DIR "/r.db", l1, 1);
    if (!r) FAIL("replica open");
    if (!root_matches_scan(r, "replica")) FAIL("replica root");
    if (!same_root(w, r)) FAIL("writer and replica roots differ");
    sqlite3_stmt *st;
    sqlite3_prepare_v2(r->db, "SELECT roots_compared, root_mismatches FROM hpplite_status", -1, &st, NULL);
    if (sqlite3_step(st) != SQLITE_ROW || sqlite3_column_int(st, 0) < 7 || sqlite3_column_int(st, 1) != 0) {
        printf("(compared %d, mismatches %d) ", sqlite3_column_int(st, 0), sqlite3_column_int(st, 1));
        sqlite3_finalize(st); FAIL("the replica compared every checkpoint root without a mismatch");
    }
    sqlite3_finalize(st);
    hpplite_node_destroy(w); hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    PASS();
}



/* A batch at the writer's next height, posted by hand with a root computed
** by `fn` and the version byte `version` (0: absent, as histories from
** before #9 wrote it). The writer's own tables are what the batch applies to. */
static int post_root_batch(HppliteNode *w, HppliteL1 *l1, uint64_t height, const char *zSql,
                           int (*fn)(sqlite3 *, unsigned char *), int version) {
    HppliteBatch *b = hpplite_batch_new(height);
    unsigned char root[32];
    size_t len = 0;
    hpplite_batch_add_stmt(b, zSql, -1, 0);
    hpplite_node_exec(w, zSql, NULL);                 /* the writer's file is the state the root is of */
    if (fn(w->db, root) != SQLITE_OK) { hpplite_batch_free(b); return 0; }
    hpplite_batch_set_post_state(b, root);
    b->rootVersion = version;
    char *json = hpplite_batch_to_json(b);
    uint8_t *sealed = hpplite_envelope_seal(w->cipher, w->encKey, height, HPPLITE_ENCODING_NONE,
                                            (const uint8_t *)json, strlen(json), &len);
    char *tx = sealed ? hpplite_l1_submit_batch(l1, w->instanceId, height, sealed, len) : NULL;
    sqlite3_free(json); hpplite_batch_free(b); free(sealed);
    if (!tx) return 0;
    sqlite3_free(tx);
    return 1;
}

static int status_ints(sqlite3 *db, int *compared, int *mismatches) {
    sqlite3_stmt *st;
    int ok = 0;
    sqlite3_prepare_v2(db, "SELECT roots_compared, root_mismatches FROM hpplite_status", -1, &st, NULL);
    if (sqlite3_step(st) == SQLITE_ROW) { *compared = sqlite3_column_int(st, 0); *mismatches = sqlite3_column_int(st, 1); ok = 1; }
    sqlite3_finalize(st);
    return ok;
}

static void test_version_1_history(void) {
    TEST("a history with version 1 roots (no root_v) is compared with version 1: no mismatch; a wrong root still is one");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer(DIR "/w.db", l1, 1000);        /* no roots of its own */
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(a, b)", NULL);
    hpplite_node_exec(w, "INSERT INTO t VALUES(1,'x'),(2,'y')", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("flush");
    /* height 2: a version 1 root, as a writer from before #9 would post it */
    if (!post_root_batch(w, l1, 2, "INSERT INTO t VALUES(3,'z')", hpplite_db_hash_v1, 0)) FAIL("post v1 batch");
    HppliteNode *r = open_writer(DIR "/r.db", l1, 1000);
    if (!r) FAIL("replica open");
    int compared = 0, mismatches = 0;
    if (!status_ints(r->db, &compared, &mismatches)) FAIL("status");
    if (compared != 1 || mismatches != 0) { printf("(compared %d, mismatches %d) ", compared, mismatches); FAIL("the version 1 root should match under version 1"); }
    hpplite_node_destroy(r);
    /* height 3: a version 2 root claimed as version 1: the replica must see the mismatch */
    if (!post_root_batch(w, l1, 3, "INSERT INTO t VALUES(4,'w')", hpplite_db_hash, 0)) FAIL("post wrong batch");
    r = open_writer(DIR "/r2.db", l1, 1000);
    if (!r) FAIL("replica 2 open");
    if (!status_ints(r->db, &compared, &mismatches)) FAIL("status 2");
    if (compared != 2 || mismatches != 1) { printf("(compared %d, mismatches %d) ", compared, mismatches); FAIL("a version 2 root under version 1 must mismatch"); }
    hpplite_node_destroy(r);
    /* height 4: a version 2 root, marked: matches */
    if (!post_root_batch(w, l1, 4, "INSERT INTO t VALUES(5,'v')", hpplite_db_hash, 2)) FAIL("post v2 batch");
    r = open_writer(DIR "/r3.db", l1, 1000);
    if (!r) FAIL("replica 3 open");
    if (!status_ints(r->db, &compared, &mismatches)) FAIL("status 3");
    if (compared != 3 || mismatches != 1) { printf("(compared %d, mismatches %d) ", compared, mismatches); FAIL("the version 2 root should match under version 2"); }
    hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* 4. A file from before #9 (#131): its meta row has no tree columns and
** there is no tree table. Opening on this build must add them, or every
** flush fails with "no such column: tree_seq" (the demo, at height 229) */
static void test_old_file_gains_trees(void) {
    TEST("a bookkeeping file from before #9 gains the tree table and columns; it posts and a replica matches (#131)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer(DIR "/old.db", l1, 1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT); INSERT INTO t VALUES(1, 'before')", NULL);
    if (hpplite_node_flush_batch(w) < 1) FAIL("flush before");
    hpplite_node_destroy(w);
    {   /* strip what #9 added: the shape of a file written by an earlier build */
        sqlite3 *raw = NULL;
        if (sqlite3_open(DIR "/old.db", &raw) != SQLITE_OK) FAIL("raw open");
        if (exec_sql(raw, "DROP TABLE _hpplite_tree;"
                          "ALTER TABLE _hpplite_meta DROP COLUMN tree_seq;"
                          "ALTER TABLE _hpplite_meta DROP COLUMN tree_stale") != SQLITE_OK) {
            printf("(%s) ", sqlite3_errmsg(raw)); sqlite3_close(raw); FAIL("strip");
        }
        sqlite3_close(raw);
    }
    w = open_writer(DIR "/old.db", l1, 1);
    if (!w) FAIL("reopen");
    if (scalar(w->ctx->bkDb, "SELECT count(*) FROM pragma_table_info('_hpplite_meta') WHERE name IN ('tree_seq','tree_stale')") != 2)
        FAIL("the tree columns were not added to the old file");
    if (scalar(w->ctx->bkDb, "SELECT count(*) FROM sqlite_master WHERE name = '_hpplite_tree'") != 1)
        FAIL("the tree table was not created in the old file");
    hpplite_node_exec(w, "INSERT INTO t VALUES(2, 'after')", NULL);
    if (hpplite_node_flush_batch(w) < 1) FAIL("flush after the upgrade");
    const char *why = test_replica_check(w, l1, DIR "/old_r.db", NULL, 0, 0, 1);
    hpplite_l1_disconnect(l1);
    if (why) FAIL(why);
    PASS();
}

/* 5. A verifier keeps the rebuilt root after every recent batch (#120): the
** chain's checkpoint may lag the last posted root (the timer still
** submitting it) or sit at a height with no posted root (a retried one);
** hpplite-verify checks it at that height instead of saying "not reached". */
static void test_root_at(void) {
    TEST("a verifier answers the rebuilt root at any recent height, posted root or not; a writer's roots match (#120)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer(DIR "/ra_w.db", l1, 2);          /* a posted root every other batch */
    unsigned char want[6][32], got[32];
    int h;
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b)", NULL);
    if (hpplite_node_flush_batch(w) < 1) FAIL("flush 1");
    hpplite_node_get_state_root(w, want[1]);
    for (h = 2; h <= 5; h++) {
        char sql[80];
        snprintf(sql, sizeof sql, "INSERT INTO t(b) VALUES('row %d')", h);
        hpplite_node_exec(w, sql, NULL);
        if (hpplite_node_flush_batch(w) < 1) FAIL("flush");
        hpplite_node_get_state_root(w, want[h]);
    }
    TestNodeOpts ro = { .verifyOnly = 1, .nodeId = "root_at_r" };
    HppliteNode *r = test_open_node(DIR "/ra_r.db", l1, &ro);
    if (!r) { hpplite_node_destroy(w); FAIL("open verifier"); }
    for (h = 1; h <= 5; h++) {
        if (hpplite_root_at(r->ctx, (uint64_t)h, got) != 0) { printf("(height %d not kept) ", h); r->l1 = NULL; hpplite_node_destroy(r); hpplite_node_destroy(w); FAIL("a recent height's root should be kept"); }
        if (memcmp(got, want[h], 32) != 0) { printf("(height %d differs) ", h); r->l1 = NULL; hpplite_node_destroy(r); hpplite_node_destroy(w); FAIL("the verifier's root at a height should be the writer's then"); }
    }
    if (hpplite_root_at(r->ctx, 99, got) == 0) { r->l1 = NULL; hpplite_node_destroy(r); hpplite_node_destroy(w); FAIL("an unknown height should not answer"); }
    if (hpplite_root_at(w->ctx, 3, got) == 0) { r->l1 = NULL; hpplite_node_destroy(r); hpplite_node_destroy(w); FAIL("a writer keeps no ring (only verifiers do)"); }
    r->l1 = NULL; hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    PASS();
}

/* #142: the trees are written once per checkpoint interval, not with every
** batch (a write of every touched node held the writer for hundreds of
** milliseconds per flush). Between writes the file says the trees are
** stale, so a copy taken then (a crash) rebuilds the same root from a scan. */
static void test_trees_written_per_checkpoint(void) {
    TEST("the trees are written at the checkpoint, not at every batch; between, the file is marked stale and a copy of it rebuilds the same root");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer(DIR "/w.db", l1, 4);          /* a root every 4 batches */
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)", NULL);
    hpplite_node_exec(w, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 200)"
                         " INSERT INTO t SELECT i, 'row ' || i FROM s; COMMIT", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    sqlite3_int64 nodes1 = scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_tree");
    sqlite3_int64 seq1 = scalar(w->ctx->bkDb, "SELECT tree_seq FROM _hpplite_meta WHERE id = 1");
    hpplite_node_exec(w, "UPDATE t SET b = 'two' WHERE a <= 50", NULL);
    if (hpplite_node_flush_batch(w) != 2) FAIL("batch 2");
    hpplite_node_exec(w, "DELETE FROM t WHERE a > 150", NULL);
    if (hpplite_node_flush_batch(w) != 3) FAIL("batch 3");
    sqlite3_int64 nodes3 = scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_tree");
    sqlite3_int64 seq3 = scalar(w->ctx->bkDb, "SELECT tree_seq FROM _hpplite_meta WHERE id = 1");
    sqlite3_int64 stale3 = scalar(w->ctx->bkDb, "SELECT tree_stale FROM _hpplite_meta WHERE id = 1");
    printf("(tree nodes after batch 1: %lld, after batch 3: %lld; tree_seq %lld -> %lld; stale %lld) ",
           (long long)nodes1, (long long)nodes3, (long long)seq1, (long long)seq3, (long long)stale3);
    if (nodes3 != nodes1 || seq3 != seq1) FAIL("batches 2 and 3 wrote the trees");
    if (stale3 != 1) FAIL("the file does not say the trees are behind the log");
    if (!root_matches_scan(w, "writer between checkpoints")) FAIL("writer root");

    /* a copy of the file as it is now: what a crash would leave */
    {
        sqlite3 *copy = NULL;
        sqlite3_backup *bk;
        if (sqlite3_open(DIR "/copy.db", &copy) != SQLITE_OK) FAIL("open copy");
        bk = sqlite3_backup_init(copy, "main", w->db, "main");
        if (!bk || sqlite3_backup_step(bk, -1) != SQLITE_DONE) FAIL("backup");
        sqlite3_backup_finish(bk);
        sqlite3_close(copy);
    }
    hpplite_test_rows_hashed = 0;
    HppliteNode *c = open_writer(DIR "/copy.db", NULL, 4);     /* no chain: the file alone */
    if (!c) FAIL("open the copy");
    if (!root_matches_scan(c, "copy")) FAIL("copy root");
    if (!same_root(w, c)) FAIL("the copy's root differs from the writer's");
    printf("(the copy rebuilt its trees from %ld rows) ", hpplite_test_rows_hashed);
    if (hpplite_test_rows_hashed == 0) FAIL("the copy did not rebuild its trees from a scan");
    hpplite_node_destroy(c);

    hpplite_node_exec(w, "INSERT INTO t(a, b) VALUES(1000, 'four')", NULL);
    if (hpplite_node_flush_batch(w) != 4) FAIL("batch 4");       /* closes the interval: a root, the trees written */
    sqlite3_int64 nodes4 = scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_tree");
    if (scalar(w->ctx->bkDb, "SELECT tree_stale FROM _hpplite_meta WHERE id = 1") != 0) FAIL("still stale after the checkpoint");
    if (nodes4 == nodes3) FAIL("the checkpoint did not write the trees");
    printf("(after the checkpoint: %lld nodes) ", (long long)nodes4);
    if (!root_matches_scan(w, "writer at the checkpoint")) FAIL("writer root at the checkpoint");
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* #142, the other way round: a bookkeeping commit that fails (a full disk)
** rolls back the stale mark and whatever the trees wrote; the in-memory
** trees must not believe any of it, or the maintained root drifts from the
** file. A short fault loop like test_faults', with the commit failures at
** every kind of batch: deferred, at the checkpoint, at the retry. */
static void test_trees_survive_failed_commits(void) {
    TEST("bookkeeping commits that fail while the trees wait in memory: the maintained root still equals the scan, and a replica agrees");
#ifndef HPPLITE_TEST_HOOKS
    printf("SKIP (no test hooks) "); PASS(); return;
#else
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_writer(DIR "/f.db", l1, 3);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(a INTEGER PRIMARY KEY, b TEXT)", NULL);
    hpplite_node_exec(w, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 300)"
                         " INSERT INTO t SELECT i, 'row ' || i FROM s; COMMIT", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    unsigned x = 12345;
    int failed = 0, step;
    for (step = 0; step < 24; step++) {
        char sql[200];
        x = x * 1103515245u + 12345u;
        switch ((x >> 16) % 3) {
            case 0: snprintf(sql, sizeof sql, "UPDATE t SET b = 'step %d' WHERE a %% 7 = %d", step, step % 7); break;
            case 1: snprintf(sql, sizeof sql, "DELETE FROM t WHERE a = %d", 1 + step * 11); break;
            default: snprintf(sql, sizeof sql, "INSERT OR REPLACE INTO t(a, b) VALUES(%d, 'new %d')", 400 + step, step); break;
        }
        hpplite_node_exec(w, sql, NULL);
        x = x * 1103515245u + 12345u;
        if ((x >> 16) % 2) { hpplite_test_fail_commits = 1; failed++; }
        hpplite_node_flush_batch(w);
        hpplite_test_fail_commits = 0;
        x = x * 1103515245u + 12345u;
        if ((x >> 16) % 2) hpplite_node_flush_batch(w);
        if (!root_matches_scan(w, "writer")) { printf("(step %d, %d commits failed so far) ", step, failed); FAIL("writer root"); break; }
    }
    for (int i = 0; i < 5 && hpplite_node_pending_count(w) > 0; i++) hpplite_node_flush_batch(w);
    if (hpplite_node_pending_count(w) != 0) FAIL("rows still pending after the retries");
    printf("(%d of %d commits failed) ", failed, step);
    if (!root_matches_scan(w, "writer at the end")) FAIL("writer root at the end");
    const char *why = test_replica_check(w, l1, DIR "/f_r.db", NULL, 0, 2, 0);
    if (why) FAIL(why);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
#endif
}

int main(void) {
    printf("=== HPPLite State Root Tests ===\n");
    test_suite_init("stateroot", NULL, DIR);
    if (test_pages_mode()) { printf("SKIP: the content root and its trees are row and text capture's; page mode's root is the file's (test_pagemode)\n"); remove_dir(); return test_report(); }
    test_incremental();
    test_matches_scan();
    test_version_1_history();
    test_old_file_gains_trees();
    test_root_at();
    test_trees_written_per_checkpoint();
    test_trees_survive_failed_commits();
    remove_dir();
    return test_report();
}
