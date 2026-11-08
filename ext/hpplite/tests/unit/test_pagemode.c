/*
** Page mode (#146): a node that captures pages through the VFS, with no hook
** on the application's connection and its bookkeeping in a sidecar.
*/
#include "test_lib.h"
#include "pages.h"
#include "pagemode.h"
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
    if (scalar(n->db, "SELECT pending FROM hpplite_status") != pending) FAILF("hpplite_status.pending says %lld, the node %d (#157)", scalar(n->db, "SELECT pending FROM hpplite_status"), pending);
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
    hpplite_pagemode_stop(s->pm); s->pm = NULL;      /* the queue dies with the process (a clean close would post it); the file and the sidecar stay */
    hpplite_node_destroy(s);
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

/* every page batch carries its root (#154) */
static void test_every_page_batch_carries_its_root(void) {
    TEST("a page batch fetched back carries the root of its trailer");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/root.db", l1, 1, 1);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush");
    unsigned char root[32]; hpplite_node_get_state_root(s, root);
    HppliteBatch *b = hpplite_node_fetch_batch(s, 1);
    if (!b) FAIL("fetch");
    int has = b->hasRoot, same = memcmp(b->postStateRoot, root, 32) == 0;
    hpplite_batch_free(b);
    hpplite_node_destroy(s); hpplite_l1_disconnect(l1);
    if (!has) FAIL("the batch has no root");
    if (!same) FAIL("the batch root is not the posted one");
    PASS();
}

/* VACUUM and VACUUM INTO run in page mode (#148): the authorizer lets VACUUM's
** own attach through; the pages it rewrote replicate like any others */
static void test_vacuum_runs(void) {
    TEST("VACUUM and VACUUM INTO run in page mode, and a replica equals the vacuumed file");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/v.db", l1, 1, 1);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(x); INSERT INTO t VALUES(randomblob(3000)); INSERT INTO t VALUES(randomblob(3000)); DELETE FROM t WHERE rowid = 1", NULL);
    int vac = sqlite3_exec(s->db, "VACUUM", NULL, NULL, NULL);
    int into = sqlite3_exec(s->db, "VACUUM INTO '" DIR "/copy.db'", NULL, NULL, NULL);
    int att = sqlite3_exec(s->db, "ATTACH '" DIR "/other.db' AS o", NULL, NULL, NULL);
    hpplite_node_exec(s, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush");
    exec_sql(s->db, "PRAGMA wal_checkpoint(TRUNCATE)");
    HppliteNode *r = open_pages_role(DIR "/vr.db", l1, 1, 1, 1);
    int same = r && files_equal_masked(DIR "/v.db", DIR "/vr.db", NULL);
    long long rows = r ? scalar(r->db, "SELECT count(*) FROM t") : -1;
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(s); hpplite_l1_disconnect(l1);
    if (vac != SQLITE_OK) FAILF("VACUUM: rc %d", vac);
    if (into != SQLITE_OK) FAILF("VACUUM INTO: rc %d", into);
    if (att != SQLITE_AUTH) FAILF("ATTACH after a VACUUM: rc %d, want SQLITE_AUTH still", att);
    if (!same || rows != 2) FAIL("the replica differs from the vacuumed file");
    PASS();
}

/* an L1 configured and not connected keeps everything pending (#156) */
static void test_no_local_finalize_with_an_l1_configured(void) {
    TEST("with l1= configured and no connection, a page-mode flush keeps the batch pending instead of finalizing locally");
    reset_dir();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof config);
    config.nodeId = "nol1"; config.dataDir = DIR; config.dbPath = DIR "/nol1.db";
    config.noEncrypt = 1;
    config.rpcUrl = "http://127.0.0.1:9"; config.hasFactory = 1;          /* an L1 named, no key: no connection comes of it */
    HppliteNode *n = hpplite_node_create(&config);
    if (!n) FAIL("open");
    hpplite_node_start(n);
    hpplite_node_exec(n, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    uint64_t h = hpplite_node_flush_batch(n);
    long long height = scalar(n->db, "SELECT height FROM hpplite_status"), pending = hpplite_node_pending_count(n);
    hpplite_node_destroy(n);
    printf("(flush %llu, height %lld, pending %lld) ", (unsigned long long)h, height, pending);
    if (h != 0 || height != 1 || pending == 0) FAIL("nothing may be finalized locally");
    PASS();
}

/* the file must be in WAL mode: an open that cannot put it there fails, saying so */
static void test_open_fails_when_wal_cannot_be_set(void) {
    TEST("page mode fails the open of a rollback-journal file another connection holds a write transaction on, naming WAL");
    reset_dir();
    sqlite3 *other = NULL;
    if (sqlite3_open(DIR "/held.db", &other) != SQLITE_OK) FAIL("plain open");
    sqlite3_exec(other, "CREATE TABLE t(x); BEGIN IMMEDIATE; INSERT INTO t VALUES(1)", NULL, NULL, NULL);
    TestNodeOpts o = { .noEncrypt = 1, .localOnly = 1 };
    HppliteNode *n = test_open_node(DIR "/held.db", NULL, &o);
    const char *why = hpplite_init_error();
    sqlite3_exec(other, "COMMIT", NULL, NULL, NULL);
    sqlite3_close(other);
    printf("(%s) ", why ? why : "no reason");
    if (n) { hpplite_node_destroy(n); FAIL("the open should fail: the file is not in WAL mode and cannot be switched"); }
    if (!why || !strstr(why, "WAL")) FAIL("the reason does not name WAL mode");
    PASS();
}

/* a named database (#160): batch 1 of a page-mode rollup carries the name */
static void test_batch_one_carries_the_name(void) {
    TEST("a named database's first page batch carries the name, later ones do not");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof config);
    config.nodeId = "named"; config.dataDir = DIR; config.dbPath = DIR "/named.db";
    config.noEncrypt = 1; config.noCompress = 1; config.dbName = "alpha";
    for (int i = 0; i < 32; i++) config.privkey[i] = (unsigned char)(i + 1);          /* a key: the lease is claimed with it */
    config.hasPrivkey = 1;
    HppliteNode *n = hpplite_node_create(&config);
    if (!n) FAIL("open");
    hpplite_node_set_l1(n, l1); hpplite_node_sync_from_l1(n, 1); hpplite_node_start(n);
    hpplite_node_exec(n, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    { int pend = hpplite_node_pending_count(n); uint64_t h = hpplite_node_flush_batch(n); if (h != 1) FAILF("flush 1 returned %llu with %d pending: %s", (unsigned long long)h, pend, n->ctx->zLastError); }
    hpplite_node_exec(n, "INSERT INTO t VALUES(2)", NULL);
    if (hpplite_node_flush_batch(n) != 2) FAIL("flush 2");
    HppliteBatch *b1 = hpplite_node_fetch_batch(n, 1), *b2 = hpplite_node_fetch_batch(n, 2);
    const char *name1 = b1 && b1->zDbName ? b1->zDbName : "(none)";
    int named2 = b2 && b2->zDbName != NULL;
    printf("(batch 1: %s) ", name1);
    int ok = strcmp(name1, "alpha") == 0;
    hpplite_batch_free(b1); hpplite_batch_free(b2);
    hpplite_node_destroy(n); hpplite_l1_disconnect(l1);
    if (!ok) FAIL("batch 1 does not carry the name");
    if (named2) FAIL("batch 2 carries the name too");
    PASS();
}

/* a plain node (not role=verify) opened fresh against a chain of page batches:
** the pages are applied through a second connection with its own connection
** switched to rollback-journal mode for the sync, and back; its reads must
** see the applied pages, not a cache from before */
static void test_fresh_plain_node_reads_the_applied_pages(void) {
    TEST("a plain node opened fresh against page batches reads the rows its sync applied");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/fp.db", l1, 1, 1);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(id INTEGER PRIMARY KEY, who, k); INSERT INTO t(who, k) VALUES('a', 1), ('a', 2)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 1");
    hpplite_node_exec(s, "INSERT INTO t(who, k) VALUES('b', 3)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 2");
    HppliteNode *r = open_pages_role(DIR "/fpr.db", l1, 1, 1, 0);
    if (!r) FAIL("open the plain node");
    long long rows = scalar(r->db, "SELECT count(*) FROM t");
    char mode[16]; text_of(r->db, "PRAGMA journal_mode", mode, sizeof mode);
    int err = r->state == HPPLITE_STATE_ERROR;
    printf("(rows %lld, mode %s%s%s) ", rows, mode, err ? ", error: " : "", err ? r->ctx->zLastError : "");
    int same = same_root(s, r);
    hpplite_node_destroy(r); hpplite_node_destroy(s); hpplite_l1_disconnect(l1);
    if (err) FAIL("the plain node is in error");
    if (rows != 3) FAIL("the plain node does not read the applied rows");
    if (!same) FAIL("roots differ");
    PASS();
}

/* a commit that dirties more pages than fit under the posting limit (#152):
** posted as a series of pieces, each under the limit, the replica holding
** its lock from the first to the last; a crash between pieces is recovered */
static void test_commit_over_the_limit_is_cut_into_pieces(void) {
    TEST("a commit larger than the posting limit is posted as pieces under the limit; a replica applies the series and equals; the next commit follows");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .noEncrypt = 1, .noCompress = 1, .maxBatchBytes = 16 * 1024, .checkpointInterval = TEST_INTERVAL_1000 };
    HppliteNode *s = test_open_node(DIR "/big.db", l1, &o);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 1");
    /* one transaction, ~30 pages of random bytes: at 16 KB a batch holds 3 */
    hpplite_node_exec(s, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 30) INSERT INTO t(v) SELECT randomblob(3500) FROM s; COMMIT", NULL);
    uint64_t last = hpplite_node_flush_batch(s);
    int pending = hpplite_node_pending_count(s);
    hpplite_node_exec(s, "INSERT INTO t(v) VALUES(x'00')", NULL);
    uint64_t after = hpplite_node_flush_batch(s);
    int pieces = 0, whole = 0; size_t biggest = 0;
    for (uint64_t h = 2; h <= after; h++) {
        size_t len = 0; uint8_t *raw = hpplite_l1_get_batch(l1, h, &len);
        HpplitePageBatch *pb = raw ? hpplite_pagebatch_decode(raw, len) : NULL;
        if (pb) { if (pb->piece) pieces++; else whole++; }
        if (len > biggest) biggest = len;
        hpplite_pagebatch_free(pb); free(raw);
    }
    HppliteNode *r = open_pages_role(DIR "/bigr.db", l1, 1, 1, 1);
    long long rows = r ? scalar(r->db, "SELECT count(*) FROM t") : -1;
    int same = r && same_root(s, r);
    exec_sql(s->db, "PRAGMA wal_checkpoint(TRUNCATE)");
    int filesEqual = r && files_equal_masked(DIR "/big.db", DIR "/bigr.db", NULL);
    printf("(series to height %llu, %d pieces, %d whole after, biggest %zu bytes, replica %lld rows, roots %s, files %s) ", (unsigned long long)last, pieces, whole, biggest, rows, same ? "equal" : "differ", filesEqual ? "equal" : "differ");
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(s); hpplite_l1_disconnect(l1);
    if (last < 5) FAILF("the commit should have been cut into several pieces, last height %llu", (unsigned long long)last);
    if (pending != 0) FAILF("%d pending after the series", pending);
    if (biggest > 16 * 1024) FAILF("a batch is %zu bytes, over the limit", biggest);
    if (pieces < 5 || whole != 1) FAIL("the pieces and the whole batch after them");
    if (after != last + 1) FAIL("the next commit did not follow as a whole batch");
    if (!r || rows != 31 || !same || !filesEqual) FAIL("the replica does not equal the sequencer after the series");
    PASS();
}

/* the process dies with a piece of a commit in flight (#152): the reopened
** sequencer takes the piece as posted and ships the file whole again; a
** reader that rebuilds from the chain, pieces and all, equals it */
static void test_death_between_pieces(void) {
    TEST("a sequencer that dies between two pieces of a commit reopens, re-ships the file whole, and a reader rebuilds the series");
#ifndef HPPLITE_TEST_HOOKS
    printf("SKIP (no test hooks) "); PASS(); return;
#else
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .noEncrypt = 1, .noCompress = 1, .maxBatchBytes = 16 * 1024, .checkpointInterval = TEST_INTERVAL_1000 };
    HppliteNode *s = test_open_node(DIR "/dp.db", l1, &o);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 1");
    hpplite_node_exec(s, "BEGIN; WITH RECURSIVE s(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM s WHERE i < 30) INSERT INTO t(v) SELECT randomblob(3500) FROM s; COMMIT", NULL);
    hpplite_test_fail_commits = 1;                    /* the third piece's finalize fails... */
    uint64_t h1 = hpplite_node_flush_batch(s);        /* ...the flush stops there: pieces 1, 2 finalized, 3 on the chain and in flight */
    hpplite_test_fail_commits = 0;
    hpplite_node_exec(s, "INSERT INTO t(v) VALUES(x'00')", NULL);
    hpplite_pagemode_stop(s->pm); s->pm = NULL;      /* the process dies */
    hpplite_node_destroy(s);
    HppliteNode *s2 = test_open_node(DIR "/dp.db", l1, &o);
    if (!s2) FAIL("reopen");
    long long h = scalar(s2->db, "SELECT height FROM hpplite_status");
    uint64_t h2 = hpplite_node_flush_batch(s2);
    printf("(flush stopped at %llu, height after reopen %lld, re-ship to %llu) ", (unsigned long long)h1, h, (unsigned long long)h2);
    HppliteNode *r = open_pages_role(DIR "/dpr.db", l1, 1, 1, 1);
    long long rows = r ? scalar(r->db, "SELECT count(*) FROM t") : -1;
    int same = r && same_root(s2, r), err = r && r->state == HPPLITE_STATE_ERROR;
    if (err) printf("(reader: %s) ", r->ctx->zLastError);
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(s2); hpplite_l1_disconnect(l1);
    if (err) FAIL("the reader is in error");
    if (rows != 31 || !same) FAILF("reader rows %lld, roots %s", rows, same ? "equal" : "differ");
    PASS();
#endif
}

/* The first batch of a rollup can be the file as opened, with no commit in it (a node opened, flushed or closed,
** wrote nothing). A second sequencer on a fresh file for that rollup - a signer's node, a machine resumed elsewhere -
** must take that image as the history it is behind and apply it, then post its own (#253; anvil 23) */
static void test_image_batch_adopted_by_a_fresh_sequencer(void) {
    TEST("a rollup whose batch 1 is the file's image with no commit: a fresh sequencer applies it and posts after it");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_pages(DIR "/img_a.db", l1, 1, 1);
    if (!a) FAIL("open a");
    long long h1 = hpplite_node_flush_batch(a);                 /* nothing written: the file as opened is the batch */
    hpplite_node_destroy(a);
    if (h1 != 1) FAILF("the empty file's image should be batch 1 (flush %lld)", h1);
    HppliteNode *b = open_pages(DIR "/img_b.db", l1, 1, 1);
    if (!b) FAIL("open b");
    int synced = hpplite_node_sync_from_l1(b, 1);
    hpplite_node_exec(b, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    long long h2 = hpplite_node_flush_batch(b);
    int err = b->state == HPPLITE_STATE_ERROR;
    if (err || h2 != 2) printf("(synced %d, flush %lld, state %s: %s) ", synced, h2, err ? "error" : "ok", b->ctx->zLastError);
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    if (err) FAIL("the fresh sequencer stopped on the image batch");
    if (h2 != 2) FAIL("the fresh sequencer should post its own batch after the image");
    PASS();
}

/* A signer's node (#17) holds no key at open: the rollup's sealed history cannot be read until the batch key is given
** apart. With page capture the node's own writes must come after that history is applied to its fresh file, so the key's
** arrival reads the chain before anything else happens (#253; anvil 23) */
static void test_key_given_later_reads_the_history_first(void) {
    TEST("a node opened without a key on a sealed history: the batch key given later reads the history first, then it posts its own");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_pages(DIR "/late_a.db", l1, 0, 1);                      /* sealed with the suite's key */
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("a posts batch 1");
    unsigned char derived[32]; memcpy(derived, a->encKey, 32);                     /* the batch key the owner would hand over */
    hpplite_node_destroy(a);
    HppliteNodeConfig config;
    memset(&config, 0, sizeof config);
    config.nodeId = "late"; config.dataDir = DIR; config.dbPath = DIR "/late_b.db"; config.noCompress = 1;
    HppliteNode *b = hpplite_node_create(&config);                                /* no key: the history is sealed to it */
    if (!b) FAIL("open b");
    hpplite_node_set_l1(b, l1); hpplite_node_sync_from_l1(b, 1); hpplite_node_start(b);
    int unreadable = b->state == HPPLITE_STATE_ERROR || hpplite_node_get_height(b) == 1;
    if (hpplite_node_set_batch_key(b, derived) != 0) FAIL("set the batch key");
    long long height = hpplite_node_get_height(b);
    hpplite_node_exec(b, "INSERT INTO t VALUES(2)", NULL);                        /* t exists: the history was applied by the key's arrival */
    long long h2 = hpplite_node_flush_batch(b);
    int err = b->state == HPPLITE_STATE_ERROR;
    long long rows = scalar(b->db, "SELECT count(*) FROM t");
    if (!unreadable || height != 2 || h2 != 2 || err || rows != 2) printf("(unreadable before %d, height after the key %lld, flush %lld, rows %lld, state %s: %s) ", unreadable, height, h2, rows, err ? "error" : "ok", b->ctx->zLastError);
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    if (!unreadable) FAIL("the history should be unreadable without the key");
    if (height != 2) FAIL("the key's arrival should read the history: height 2");
    if (err || h2 != 2 || rows != 2) FAIL("the node should post its own batch after the history, on the history");
    PASS();
}

/* Histories from v0.7 and earlier are statements or rows: a file carrying HPPLite's tables in itself, or a rollup
** whose batch 1 is not a page batch. Neither is read since v0.8 (#253): the open, or the sync, refuses and says so */
static void test_rows_history_refused(void) {
    TEST("a file with a v0.7 log inside it is refused at open; a rollup whose batch 1 is not a page batch stops the replica");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof config);
    config.nodeId = "old"; config.dataDir = DIR; config.dbPath = DIR "/old_rows.db"; config.noEncrypt = 1;
    for (int i = 0; i < 32; i++) config.privkey[i] = (unsigned char)(i + 1);
    config.hasPrivkey = 1;
    {   /* a v0.7 file: the meta table in the database itself */
        sqlite3 *plain = NULL;
        sqlite3_open(DIR "/old_rows.db", &plain);
        sqlite3_exec(plain, "CREATE TABLE _hpplite_meta(id INTEGER PRIMARY KEY, next_height INTEGER); INSERT INTO _hpplite_meta VALUES(1, 5)", NULL, NULL, NULL);
        sqlite3_close(plain);
    }
    HppliteNode *w = hpplite_node_create(&config);
    if (w) { hpplite_node_destroy(w); FAIL("a file with a rows/statements log opened"); }
    if (!hpplite_init_error() || !strstr(hpplite_init_error(), "statements or rows")) FAIL("the refusal should name the history's kind");
    /* a chain whose batch 1 is plaintext JSON (a statements batch): the replica stops at it and says so */
    const char *json = "{\"height\":1,\"txns\":[{\"sql\":\"CREATE TABLE t(x)\"}]}";
    hpplite_l1_mock_inject_batch(l1, (const uint8_t *)json, strlen(json));
    config.nodeId = "oldr"; config.dbPath = DIR "/old_replica.db";
    HppliteNode *r = hpplite_node_create(&config);
    if (!r) FAIL("open the replica");
    hpplite_node_set_l1(r, l1);
    int synced = hpplite_node_sync_from_l1(r, 1);
    int err = r->state == HPPLITE_STATE_ERROR;
    const char *why = r->ctx->zLastError;
    int named = strstr(why, "not a page batch") != NULL;
    if (!err || synced >= 0 || !named) printf("(synced %d, error %d: %s) ", synced, err, why);
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    if (!err || synced >= 0) FAIL("a statements batch should stop the replica");
    if (!named) FAIL("the error should say the batch is not a page batch");
    PASS();
}

/* a writer without HPPLite (another process, say) commits, checkpoints and
** writes again: SQLite restarts the WAL once it is fully backfilled, and the
** sequencer must have read every frame before that can happen. Its pinned
** read transaction keeps the WAL from being fully backfilled past what it
** read (#153); here the sequencer takes no tick between the burst and the
** restart, so only the pin stands between it and a lost thousand commits */
static void test_foreign_checkpoint_and_restart_lose_nothing(void) {
    TEST("a thousand commits, a checkpoint and a WAL restart by a connection without HPPLite, with no tick of the sequencer between: nothing is lost");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNodeConfig config;
    memset(&config, 0, sizeof config);
    config.nodeId = "fc"; config.dataDir = DIR; config.dbPath = DIR "/fc.db"; config.noEncrypt = 1; config.noCompress = 1;
    config.checkpointInterval = TEST_INTERVAL_1000; config.batchIntervalMs = 3600 * 1000;   /* a timer: its ticks checkpoint and move the pin, the interval never comes */
    for (int i = 0; i < 32; i++) config.privkey[i] = (unsigned char)(i + 1);
    config.hasPrivkey = 1;
    HppliteNode *s = hpplite_node_create(&config);
    if (!s) FAIL("open");
    hpplite_node_set_l1(s, l1); hpplite_node_sync_from_l1(s, 1); hpplite_node_start(s); hpplite_node_start_timer(s);
    hpplite_node_exec(s, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 1");
    usleep(1500000);                                  /* ticks: the WAL checkpointed whole (the checkpoint runs once a second), the pin moved after that */
    hpplite_node_stop_timer(s);                       /* and no tick from here to the flush: the pin as it is must hold */
    sqlite3 *c = NULL;
    if (sqlite3_open(DIR "/fc.db", &c) != SQLITE_OK) FAIL("plain open");
    sqlite3_busy_timeout(c, 5000);
    for (int i = 0; i < 1100; i++) {
        char sql[64]; snprintf(sql, sizeof sql, "INSERT INTO t(v) VALUES(%d)", i);
        if (sqlite3_exec(c, sql, NULL, NULL, NULL) != SQLITE_OK) FAIL("plain write");
    }
    int ckLog = -1, ckDone = -1;
    sqlite3_wal_checkpoint_v2(c, "main", SQLITE_CHECKPOINT_PASSIVE, &ckLog, &ckDone);     /* what a plain process's auto-checkpoint does */
    if (sqlite3_exec(c, "INSERT INTO t(v) VALUES(-1)", NULL, NULL, NULL) != SQLITE_OK) FAIL("plain write after the checkpoint");
    sqlite3_close(c);
    uint64_t h = hpplite_node_flush_batch(s);
    HppliteNode *r = open_pages_role(DIR "/fcr.db", l1, 1, 1, 1);
    long long rows = r ? scalar(r->db, "SELECT count(*) FROM t") : -1;
    int same = r && same_root(s, r);
    printf("(checkpoint backfilled %d of %d frames; flush to %llu; replica %lld rows, roots %s) ", ckDone, ckLog, (unsigned long long)h, rows, same ? "equal" : "differ");
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(s); hpplite_l1_disconnect(l1);
    if (ckDone >= ckLog) FAIL("the plain checkpoint backfilled the whole WAL: the sequencer's pin did not hold");
    if (rows != 1101 || !same) FAIL("commits were lost");
    PASS();
}

/* what another connection or process committed is pending the moment it is
** asked about, not at the next tick: the count polls the WAL (#153) */
static void test_pending_counts_foreign_commits(void) {
    TEST("commits by a connection without HPPLite count as pending at once, and a close posts them");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/fp.db", l1, 1, 1);
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    if (hpplite_node_flush_batch(s) == 0) FAIL("flush 1");
    sqlite3 *c = NULL;
    if (sqlite3_open(DIR "/fp.db", &c) != SQLITE_OK) FAIL("plain open");
    for (int i = 0; i < 5; i++) if (sqlite3_exec(c, "INSERT INTO t(v) VALUES(1)", NULL, NULL, NULL) != SQLITE_OK) FAIL("plain write");
    sqlite3_close(c);
    int pending = hpplite_node_pending_count(s);
    hpplite_node_destroy(s);                          /* a clean close posts what is pending */
    HppliteNode *r = open_pages_role(DIR "/fpr.db", l1, 1, 1, 1);
    long long rows = r ? scalar(r->db, "SELECT count(*) FROM t") : -1;
    if (r) hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    printf("(pending %d, replica %lld rows) ", pending, rows);
    if (pending < 5) FAIL("the other connection's commits did not count as pending");
    if (rows != 5) FAIL("the close did not post them");
    PASS();
}

/* #174: a second connection on a page-mode file (the demo's role=verify one, opened while the
** sequencer holds the file) captures nothing itself, but the file is a page-mode file and its
** status says so - not the connection's default */
static void test_follower_reports_pages(void) {
    TEST("a verify connection on a page-mode file the sequencer holds reports capture = pages (#174)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/held_pages.db", l1, 1, 1);
    if (!s) FAIL("open the sequencer");
    hpplite_node_exec(s, "CREATE TABLE t(x); INSERT INTO t VALUES(1)", NULL);
    if (hpplite_node_flush_batch(s) != 1) FAIL("flush");
    HppliteNodeConfig config;
    char cap[16];
    memset(&config, 0, sizeof config);
    config.nodeId = "vf"; config.dataDir = DIR; config.dbPath = DIR "/held_pages.db";
    config.verifyOnly = 1;
    HppliteNode *v = hpplite_node_create(&config);
    if (!v) FAIL("open the verify connection");
    hpplite_node_set_l1(v, l1); hpplite_node_sync_from_l1(v, 1); hpplite_node_start(v);
    text_of(v->db, "SELECT capture FROM hpplite_status", cap, sizeof cap);
    int follower = v->follower, hasPm = v->pm != NULL;
    hpplite_node_destroy(v);
    hpplite_node_destroy(s);
    hpplite_l1_disconnect(l1);
    printf("(follower %d, page machinery %d, capture %s) ", follower, hasPm, cap);
    if (strcmp(cap, "pages") != 0) FAILF("hpplite_status.capture = %s on a page-mode file", cap);
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
    test_every_page_batch_carries_its_root();
    test_vacuum_runs();
    test_no_local_finalize_with_an_l1_configured();
    test_open_fails_when_wal_cannot_be_set();
    test_batch_one_carries_the_name();
    test_fresh_plain_node_reads_the_applied_pages();
    test_commit_over_the_limit_is_cut_into_pieces();
    test_death_between_pieces();
    test_rows_history_refused();
    test_image_batch_adopted_by_a_fresh_sequencer();
    test_key_given_later_reads_the_history_first();
    test_foreign_checkpoint_and_restart_lose_nothing();
    test_pending_counts_foreign_commits();
    test_follower_reports_pages();
    remove_dir();
    return test_report();
}
