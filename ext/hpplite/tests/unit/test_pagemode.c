/*
** Page mode (#146): a node that captures pages through the VFS, with no hook
** on the application's connection and its bookkeeping in a sidecar.
*/
#include "test_lib.h"
#include "pages.h"
#include <sys/stat.h>

#define DIR "/tmp/hpplite_test_pagemode"

static HppliteNode *open_pages(const char *dbPath, HppliteL1 *l1, int noEncrypt, int noCompress) {
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
    if (pending != 3) FAILF("pending = %d commits, expected 3", pending);
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

int main(void) {
    printf("=== HPPLite page mode ===\n");
    test_suite_init("pagemode", NULL, DIR);
    test_no_hooks_sidecar_and_roots();
    test_posted_bytes_are_page_batches();
    remove_dir();
    return test_report();
}
