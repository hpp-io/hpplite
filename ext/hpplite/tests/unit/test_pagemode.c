/*
** Page mode (#146): a node that captures pages through the VFS, with no hook
** on the application's connection and its bookkeeping in a sidecar.
*/
#include "test_lib.h"
#include "pages.h"
#include <sys/stat.h>
#include <unistd.h>

#define DIR "/tmp/hpplite_test_pagemode"

static HppliteNode *open_pages_role(const char *dbPath, HppliteL1 *l1, int noEncrypt, int noCompress, int verifyOnly);
static HppliteNode *open_pages(const char *dbPath, HppliteL1 *l1, int noEncrypt, int noCompress) { return open_pages_role(dbPath, l1, noEncrypt, noCompress, 0); }

static HppliteNode *open_pages_role(const char *dbPath, HppliteL1 *l1, int noEncrypt, int noCompress, int verifyOnly) {
    HppliteNodeConfig config;
    HppliteNode *n;
    memset(&config, 0, sizeof(config));
    config.nodeId = "pages";
    memcpy(config.privkey, (const unsigned char *)"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\x20", 32);
    config.hasPrivkey = 1;
    config.dataDir = (char *)DIR;
    config.dbPath = (char *)dbPath;
    config.capturePages = 1;
    config.noEncrypt = noEncrypt;
    config.noCompress = noCompress;
    config.checkpointInterval = 1000;
    config.localOnly = l1 == NULL;
    config.verifyOnly = verifyOnly;
    n = hpplite_node_create(&config);
    if (n && l1) { hpplite_node_set_l1(n, l1); hpplite_node_sync_from_l1(n, 1); hpplite_node_start(n); }
    else if (n) hpplite_node_start(n);
    return n;
}
static int file_exists(const char *p) { struct stat st; return stat(p, &st) == 0; }

static void test_no_hooks_sidecar_and_roots(void) {
    TEST("page mode: no log table in the file, the bookkeeping in a sidecar, pending counts commits, the posted root equals the file's");
    reset_dir();
    HppliteNode *n = open_pages(DIR "/p.db", NULL, 1, 1);
    if (!n) FAIL("open");
    if (count_rows(n->db, "_hpplite_pending") != -1) FAIL("the application's file has a log table");
    if (!file_exists(DIR "/p.db-hpplite.db")) FAIL("no sidecar");
    if (count_rows(n->bkDb, "_hpplite_meta") != 1) FAIL("the sidecar has no meta row");
    char cap[16]; text_of(n->db, "SELECT capture FROM hpplite_status", cap, sizeof cap);
    if (strcmp(cap, "pages") != 0) FAILF("hpplite_status.capture = %s", cap);
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    hpplite_node_exec(n, "BEGIN; INSERT INTO t(v) VALUES('a'); INSERT INTO t(v) VALUES('b'); COMMIT", NULL);
    hpplite_node_exec(n, "INSERT INTO t(v) VALUES('c')", NULL);
    int pending = hpplite_node_pending_count(n);
    if (pending != 4) FAILF("pending = %d, expected 4: the file as opened (its first page, never shipped) and three commits", pending);
    uint64_t h = hpplite_node_flush_batch(n);
    if (h != 1) FAILF("flush posted height %llu, expected 1", (unsigned long long)h);
    if (hpplite_node_pending_count(n) != 0) FAIL("commits still pending after the flush");
    if (scalar(n->db, "SELECT height FROM hpplite_status") != 2) FAIL("height after the first batch is not 2");
    if (scalar(n->db, "SELECT root_version FROM hpplite_status") != 4) FAIL("root_version is not 4");
    /* the posted root is the root of the file as it is */
    unsigned char kept[32], scan[32];
    hpplite_node_get_state_root(n, kept);
    exec_sql(n->db, "PRAGMA wal_checkpoint(TRUNCATE)");
    HpplitePageRoot *pr = hpplite_pageroot_from_file(DIR "/p.db", NULL);
    if (!pr) FAIL("scan");
    hpplite_pageroot_get(pr, scan); hpplite_pageroot_free(pr);
    if (memcmp(kept, scan, 32) != 0) FAIL("the kept root differs from the file's");
    /* a second batch after more writes, and the file's root again */
    hpplite_node_exec(n, "UPDATE t SET v = v || '!' WHERE id = 1; DELETE FROM t WHERE id = 2", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("second flush");
    hpplite_node_get_state_root(n, kept);
    exec_sql(n->db, "PRAGMA wal_checkpoint(TRUNCATE)");
    pr = hpplite_pageroot_from_file(DIR "/p.db", NULL);
    hpplite_pageroot_get(pr, scan); hpplite_pageroot_free(pr);
    if (memcmp(kept, scan, 32) != 0) FAIL("the kept root differs from the file's after the second batch");
    hpplite_node_destroy(n);
    PASS();
}

static void test_posted_bytes_are_page_batches(void) {
    TEST("what reaches the chain decodes as page batches: full pages first, diffs after, roots in the trailers");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *n = open_pages(DIR "/q.db", l1, 1, 1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t(v) VALUES(zeroblob(3000))", NULL);
    if (hpplite_node_flush_batch(n) != 1) FAIL("flush 1");
    hpplite_node_exec(n, "UPDATE t SET v = 'changed' WHERE id = 1", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("flush 2");
    size_t len1 = 0, len2 = 0;
    uint8_t *b1 = hpplite_l1_get_batch(l1, 1, &len1), *b2 = hpplite_l1_get_batch(l1, 2, &len2);
    if (!b1 || !b2) FAIL("the chain has no batch bytes");
    HpplitePageBatch *p1 = hpplite_pagebatch_decode(b1, len1), *p2 = hpplite_pagebatch_decode(b2, len2);
    if (!p1 || !p2) FAIL("the posted bytes are not page batches");
    unsigned i, full1 = 0, diff2 = 0;
    for (i = 0; i < p1->nPages; i++) if (p1->aPage[i].flags == HPPLITE_PAGE_FULL) full1++;
    for (i = 0; i < p2->nPages; i++) if (p2->aPage[i].flags & HPPLITE_PAGE_DIFF) diff2++;
    printf("(batch 1: %u pages, %u full; batch 2: %u pages, %u diffs; %zu + %zu bytes) ", p1->nPages, full1, p2->nPages, diff2, len1, len2);
    unsigned char kept[32]; hpplite_node_get_state_root(n, kept);
    printf("(commits %llu-%llu, %llu-%llu) ", (unsigned long long)p1->firstCommit, (unsigned long long)p1->lastCommit, (unsigned long long)p2->firstCommit, (unsigned long long)p2->lastCommit);
    int rootOk = memcmp(kept, p2->root, 32) == 0 && p1->firstCommit == 1 && p2->firstCommit == p1->lastCommit + 1 && p2->lastCommit >= p2->firstCommit;
    hpplite_pagebatch_free(p1); hpplite_pagebatch_free(p2); free(b1); free(b2);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    if (full1 == 0) FAIL("the first batch ships no full page (a page new since the start)");   /* page 1 existed at the start: a diff */
    if (diff2 == 0) FAIL("the second batch carries no diff");
    if (!rootOk) FAIL("the trailer's root or the commit range is wrong");
    PASS();
}

static unsigned char *read_whole(const char *path, size_t *pLen) {
    FILE *f = fopen(path, "rb"); unsigned char *buf; long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 1); if (fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; } fclose(f);
    *pLen = (size_t)n; return buf;
}
/* the two files equal but for page 1's mode bytes and change counters */
static int files_equal_masked(const char *a, const char *b, size_t *pLen) {
    size_t la = 0, lb = 0; unsigned char *fa = read_whole(a, &la), *fb = read_whole(b, &lb);
    int same = fa && fb && la == lb;
    if (same) {
        fa[18] = fb[18] = fa[19] = fb[19] = 0;
        memset(fa + 24, 0, 4); memset(fb + 24, 0, 4); memset(fa + 92, 0, 4); memset(fb + 92, 0, 4);
        same = memcmp(fa, fb, la) == 0;
    }
    if (pLen) *pLen = la;
    free(fa); free(fb);
    return same;
}

static void test_replica_applies_pages(void) {
    TEST("a replica rebuilt from the chain equals the sequencer's file, and keeps up as it posts more; its readers see the rows");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/s.db", l1, 0, 0);                     /* sealed and deflated, as in production */
    if (!s) FAIL("open the sequencer");
    hpplite_node_exec(s, "CREATE TABLE t(id INTEGER PRIMARY KEY, k INTEGER, v TEXT); CREATE INDEX tk ON t(k)", NULL);
    for (int i = 0; i < 6; i++) {
        char sql[160];
        snprintf(sql, sizeof sql, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 300) INSERT INTO t(k, v) SELECT i * %d, 'row ' || i FROM s; COMMIT", i + 1);
        hpplite_node_exec(s, sql, NULL);
        if (i % 2) hpplite_node_exec(s, "DELETE FROM t WHERE id % 7 = 0", NULL);
        if (hpplite_node_flush_batch(s) == 0) FAIL("flush");
    }
    exec_sql(s->db, "PRAGMA wal_checkpoint(TRUNCATE)");
    HppliteNode *r = open_pages_role(DIR "/r.db", l1, 0, 0, 1);              /* a verifier: rebuilt from the chain alone */
    if (!r) FAIL("open the replica");
    size_t len = 0;
    int same = files_equal_masked(DIR "/s.db", DIR "/r.db", &len);
    long long compared = scalar(r->db, "SELECT roots_compared FROM hpplite_status"), mism = scalar(r->db, "SELECT root_mismatches FROM hpplite_status");
    long long rowsS = scalar(s->db, "SELECT count(*) FROM t"), rowsR = scalar(r->db, "SELECT count(*) FROM t");
    printf("(%zu bytes, %lld roots compared, %lld mismatches, %lld rows) ", len, compared, mism, rowsR);
    if (!same) FAIL("the replica's file differs from the sequencer's");
    if (compared < 6 || mism != 0) FAIL("roots");
    if (rowsS != rowsR) FAIL("the replica's connection does not see the rows");
    if (!same_root(s, r)) FAIL("the state roots differ");
    /* more on the sequencer, the replica syncs again: its open connection must see the change */
    hpplite_node_exec(s, "UPDATE t SET v = 'later' WHERE id % 5 = 0; INSERT INTO t(k, v) VALUES(1, 'new')", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 7");
    exec_sql(s->db, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (hpplite_node_sync_from_l1(r, 1) < 1) FAIL("the replica did not take the new batch");
    if (!files_equal_masked(DIR "/s.db", DIR "/r.db", NULL)) FAIL("the replica differs after the second sync");
    if (scalar(r->db, "SELECT count(*) FROM t WHERE v = 'later'") != scalar(s->db, "SELECT count(*) FROM t WHERE v = 'later'")) FAIL("the replica's reader did not see the applied pages");
    hpplite_node_destroy(r);
    hpplite_node_destroy(s);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_sequencer_adopts_its_own_page_batch(void) {
    TEST("a page batch posted but not finalized (the commit failed) is adopted at the next flush from the same commits");
#ifndef HPPLITE_TEST_HOOKS
    printf("SKIP (no test hooks) "); PASS(); return;
#else
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/a.db", l1, 1, 1);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(x); INSERT INTO t VALUES(1); INSERT INTO t VALUES(2)", NULL);
    hpplite_test_fail_commits = 1;
    uint64_t h = hpplite_node_flush_batch(s);                                /* posted to the chain, the finalize's commit fails */
    hpplite_test_fail_commits = 0;
    uint64_t last = 0, total = 0;
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    if (h != 0 || last != 1) FAILF("after the failed commit: flush returned %llu, chain at %llu", (unsigned long long)h, (unsigned long long)last);
    if (hpplite_node_pending_count(s) != 4) FAILF("the commits are not back in the queue (%d pending, expected the snapshot and three)", hpplite_node_pending_count(s));
    hpplite_node_exec(s, "INSERT INTO t VALUES(3)", NULL);
    h = hpplite_node_flush_batch(s);                                          /* adopts batch 1 from the chain, posts batch 2 */
    if (scalar(s->db, "SELECT height FROM hpplite_status") != 3) FAILF("height %lld after the adoption and the next batch", (long long)scalar(s->db, "SELECT height FROM hpplite_status"));
    if (s->state == HPPLITE_STATE_ERROR) FAIL("the node is in error");
    HppliteNode *r = open_pages_role(DIR "/ar.db", l1, 1, 1, 1);
    if (!r) FAIL("open the replica");
    if (!same_root(s, r)) FAIL("a replica rebuilt from the chain has another root");
    if (scalar(r->db, "SELECT count(*) FROM t") != 3) FAIL("the replica lacks rows");
    hpplite_node_destroy(r); hpplite_node_destroy(s); hpplite_l1_disconnect(l1);
    PASS();
#endif
}

/* the process dies after the submit (the sidecar knows the batch, the queue is
** gone); the file is reopened, more is written, posted, and a verifier agrees */
static void test_reopen_after_a_submit_the_process_did_not_survive(void) {
    TEST("a sequencer reopened after dying between submit and finalize takes its batch as posted, re-ships what the file holds beyond it, and a verifier agrees");
#ifndef HPPLITE_TEST_HOOKS
    printf("SKIP (no test hooks) "); PASS(); return;
#else
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/c.db", l1, 1, 1);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(x); INSERT INTO t VALUES(1); INSERT INTO t VALUES(2)", NULL);
    hpplite_test_fail_commits = 1;
    hpplite_node_flush_batch(s);                      /* batch 1 on the chain; the finalize's commit fails */
    hpplite_test_fail_commits = 0;
    hpplite_node_exec(s, "INSERT INTO t VALUES(3)", NULL);          /* after the "crash point", before the process dies */
    unsigned char rootFile1[32];
    { exec_sql(s->db, "PRAGMA wal_checkpoint(TRUNCATE)"); HpplitePageRoot *pr = hpplite_pageroot_from_file(DIR "/c.db", NULL); hpplite_pageroot_get(pr, rootFile1); hpplite_pageroot_free(pr); }
    hpplite_node_destroy(s);                          /* the queue dies with the process; the file and the sidecar stay */
    HppliteNode *s2 = open_pages(DIR "/c.db", l1, 1, 1);
    if (!s2) FAIL("reopen");
    long long h = scalar(s2->db, "SELECT height FROM hpplite_status");
    printf("(height after reopen %lld, pending %d) ", h, hpplite_node_pending_count(s2));
    if (hpplite_node_pending_count(s2) != 1) FAILF("after the reopen the file beyond the posted batch should count as one pending snapshot, not %d", hpplite_node_pending_count(s2));
    if (h != 2) FAILF("expected height 2 after adopting batch 1, got %lld", h);
    hpplite_node_exec(s2, "INSERT INTO t VALUES(4)", NULL);
    if (hpplite_node_flush_batch(s2) < 2) FAIL("flush after reopen");
    unsigned char rootS[32]; hpplite_node_get_state_root(s2, rootS);
    { exec_sql(s2->db, "PRAGMA wal_checkpoint(TRUNCATE)"); HpplitePageRoot *pr = hpplite_pageroot_from_file(DIR "/c.db", NULL); unsigned char rf[32]; hpplite_pageroot_get(pr, rf); hpplite_pageroot_free(pr); if (memcmp(rf, rootS, 32) != 0) FAIL("the sequencer's kept root is not its file's"); }
    HppliteNode *r = open_pages_role(DIR "/cr.db", l1, 1, 1, 0);        /* a plain node catching up, as a second machine would: applies, then may post */
    if (!r) FAIL("open the second node");
    if (r->state == HPPLITE_STATE_ERROR) FAILF("the second node is in error: %s", r->ctx->zLastError);
    long long rowsS = scalar(s2->db, "SELECT count(*) FROM t"), rowsR = scalar(r->db, "SELECT count(*) FROM t");
    printf("(rows %lld vs %lld, mismatches %lld) ", rowsS, rowsR, scalar(r->db, "SELECT root_mismatches FROM hpplite_status"));
    if (rowsS != 4 || rowsR != 4) FAIL("rows");
    if (!same_root(s2, r)) FAIL("roots differ");
    hpplite_node_destroy(r); hpplite_node_destroy(s2); hpplite_l1_disconnect(l1);
    PASS();
#endif
}

/* a replica's file deleted (or restored from elsewhere) while its sidecar
** stayed: the next batch's diffs would land on the wrong pages; the node
** says so, naming the sidecar, instead of a bare root mismatch */
static void test_file_not_where_its_sidecar_says(void) {
    TEST("a replica whose file is gone but whose sidecar stayed refuses the next batch, naming the sidecar");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/s.db", l1, 1, 1);
    if (!s) FAIL("open the sequencer");
    hpplite_node_exec(s, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 1");
    HppliteNode *r = open_pages_role(DIR "/r.db", l1, 1, 1, 1);
    if (!r) FAIL("open the replica");
    if (r->state == HPPLITE_STATE_ERROR) FAILF("the replica is in error after batch 1: %s", r->ctx->zLastError);
    hpplite_node_destroy(r);
    unlink(DIR "/r.db"); unlink(DIR "/r.db-wal"); unlink(DIR "/r.db-shm");     /* the file goes, r.db-hpplite.db stays */
    hpplite_node_exec(s, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 2");
    r = open_pages_role(DIR "/r.db", l1, 1, 1, 1);
    if (!r) FAIL("reopen the replica");
    printf("(%s) ", r->ctx->zLastError);
    if (r->state != HPPLITE_STATE_ERROR) FAIL("the replica took batch 2 onto a file that is not at batch 1");
    if (!strstr(r->ctx->zLastError, "-hpplite.db")) FAIL("the error does not name the sidecar");
    hpplite_node_destroy(r); hpplite_node_destroy(s); hpplite_l1_disconnect(l1);
    PASS();
}

/* the rules that keep page capture possible (#148): the file stays in WAL
** (the frames are what is watched), ATTACH is refused; a write after a
** refused PRAGMA is still captured */
static void test_rules_that_keep_capture_possible(void) {
    TEST("page mode refuses leaving WAL and ATTACH with the reason, and captures what follows");
    reset_dir();
    HppliteNode *n = open_pages(DIR "/rules.db", NULL, 1, 1);
    if (!n) FAIL("open");
    hpplite_node_exec(n, "CREATE TABLE t(x)", NULL);
    int walRc = sqlite3_exec(n->db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
    int offRc = sqlite3_exec(n->db, "PRAGMA journal_mode=DELETE", NULL, NULL, NULL);
    int attRc = sqlite3_exec(n->db, "ATTACH DATABASE '" DIR "/other.db' AS o", NULL, NULL, NULL);
    char why[256]; text_of(n->db, "SELECT last_refusal FROM hpplite_status", why, sizeof why);
    char mode[16]; text_of(n->db, "PRAGMA journal_mode", mode, sizeof mode);
    int before = hpplite_node_pending_count(n);
    hpplite_node_exec(n, "INSERT INTO t VALUES(1)", NULL);
    int after = hpplite_node_pending_count(n);
    printf("(mode %s, refusal: %s) ", mode, why);
    hpplite_node_destroy(n);
    if (walRc != SQLITE_OK) FAIL("setting WAL again must be fine");
    if (offRc != SQLITE_AUTH) FAILF("leaving WAL: rc %d, want SQLITE_AUTH", offRc);
    if (attRc != SQLITE_AUTH) FAILF("ATTACH: rc %d, want SQLITE_AUTH", attRc);
    if (strcasecmp(mode, "wal") != 0) FAIL("the file left WAL");
    if (!strstr(why, "ATTACH")) FAIL("last_refusal does not say why");
    if (after != before + 1) FAIL("the write after the refusals was not captured");
    PASS();
}

/* a file SQLite cannot read fails the open with the reason (#149) */
static void test_unreadable_file_fails_the_open(void) {
    TEST("page mode fails the open of a file this SQLite cannot read, saying so");
    reset_dir();
    unsigned char junk[8192];
    for (size_t i = 0; i < sizeof junk; i++) junk[i] = (unsigned char)(i * 131 + 7);
    FILE *f = fopen(DIR "/junk.db", "wb");
    if (!f) FAIL("write");
    fwrite(junk, 1, sizeof junk, f);
    fclose(f);
    HppliteNode *n = open_pages(DIR "/junk.db", NULL, 1, 1);
    const char *why = hpplite_init_error();
    printf("(%s) ", why ? why : "no reason");
    if (n) { hpplite_node_destroy(n); FAIL("the open should fail: page mode cannot capture a file it cannot read"); }
    if (!why || !strstr(why, "not a database")) FAIL("the reason does not say the file is not a database");
    PASS();
}

int main(void) {
    printf("=== HPPLite page mode ===\n");
    test_suite_init("pagemode", NULL, DIR);
    test_no_hooks_sidecar_and_roots();
    test_posted_bytes_are_page_batches();
    test_replica_applies_pages();
    test_sequencer_adopts_its_own_page_batch();
    test_reopen_after_a_submit_the_process_did_not_survive();
    test_file_not_where_its_sidecar_says();
    test_rules_that_keep_capture_possible();
    test_unreadable_file_fails_the_open();
    remove_dir();
    return test_report();
}
