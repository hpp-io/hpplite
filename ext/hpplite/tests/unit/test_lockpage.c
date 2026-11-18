/*
** SQLite's lock-byte page (#363): the page holding the pending byte (offset
** 0x40000000: page 262,145 at 4 KB pages) is never written by SQLite, so it is
** never in a commit. A database past it must still have an image (a snapshot),
** and every reader must compute the sequencer's root. The pending byte is moved
** to 256 KiB (page 65) before any connection opens, so a few hundred KB cross it.
*/
#include "test_lib.h"
#include "pages.h"
#include "pagemode.h"
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define DIR "/tmp/hpplite_test_lockpage"
#define PENDING 0x40000

static HppliteNode *open_pages(const char *dbPath, HppliteL1 *l1, int verifyOnly) {
    HppliteNodeConfig config;
    HppliteNode *n;
    memset(&config, 0, sizeof(config));
    config.nodeId = "lockpage";
    memcpy(config.privkey, (const unsigned char *)"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\x1a\x1b\x1c\x1d\x1e\x1f\x20", 32);
    config.hasPrivkey = 1;
    config.dataDir = (char *)DIR;
    config.dbPath = (char *)dbPath;
    config.noEncrypt = 1;
    config.noCompress = 1;
    config.checkpointInterval = 1000;
    config.verifyOnly = verifyOnly;
    n = hpplite_node_create(&config);
    if (n) { hpplite_node_set_l1(n, l1); hpplite_node_sync_from_l1(n, 1); hpplite_node_start(n); }
    return n;
}

static int file_root(const char *zPath, unsigned char out[32]) {
    HpplitePageRoot *pr = hpplite_pageroot_from_file(zPath, NULL);
    if (!pr) return -1;
    hpplite_pageroot_get(pr, out);
    hpplite_pageroot_free(pr);
    return 0;
}

static void test_past_the_lock_page(void) {
    TEST("a database past SQLite's lock-byte page: its image is built, a reader of the file, a restarted sequencer and a replica all compute the sequencer's root (#363)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *s = open_pages(DIR "/s.db", l1, 0), *s2 = NULL, *r = NULL;
    unsigned char kept[32], img[32], scan[32], again[32];
    unsigned char *image = NULL;
    size_t len = 0;
    int i, prc;
    long long pages;
    if (!s) FAIL("open");
    hpplite_node_exec(s, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    for (i = 0; i < 10; i++) hpplite_node_exec(s, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 12) INSERT INTO t(v) SELECT randomblob(3000) FROM c", NULL);
    while (hpplite_node_pending_count(s) > 0) if (hpplite_node_flush_batch(s) == 0) FAIL("flush");
    pages = scalar(s->db, "PRAGMA page_count");
    if (pages <= PENDING / 4096 + 1) FAILF("only %lld pages: the lock page (%d) not crossed", pages, PENDING / 4096 + 1);
    hpplite_node_get_state_root(s, kept);
    prc = hpplite_pagemode_image(s->pm, &image, &len, img);
    sqlite3_free(image);
    exec_sql(s->db, "PRAGMA wal_checkpoint(TRUNCATE)");
    if (file_root(DIR "/s.db", scan) != 0) FAIL("the file's root");
    printf("(%lld pages; image rc %d, image root same %d, file root same %d) ", pages, prc, prc == SQLITE_OK && memcmp(img, kept, 32) == 0, memcmp(scan, kept, 32) == 0);
    if (prc != SQLITE_OK) FAIL("no image past the lock page: no snapshot would be made");
    if (memcmp(img, kept, 32) != 0) FAIL("the image's root is not the sequencer's");
    if (memcmp(scan, kept, 32) != 0) FAIL("the file's root (as a verifier reads it) is not the sequencer's");
    hpplite_node_destroy(s);
    s2 = open_pages(DIR "/s.db", l1, 0);
    if (!s2 || s2->state == HPPLITE_STATE_ERROR) FAILF("reopen: %s", s2 ? s2->ctx->zLastError : "no node");
    hpplite_node_get_state_root(s2, again);
    if (memcmp(again, kept, 32) != 0) FAIL("the restarted sequencer's root is not the one it anchored");
    hpplite_node_exec(s2, "INSERT INTO t(v) VALUES('after')", NULL);
    if (hpplite_node_flush_batch(s2) == 0) FAILF("a batch after the restart: %s", s2->ctx->zLastError);
    r = open_pages(DIR "/r.db", l1, 1);
    if (!r || r->state == HPPLITE_STATE_ERROR) FAILF("the replica: %s", r ? r->ctx->zLastError : "no node");
    if (scalar(r->db, "SELECT count(*) FROM t") != 121) FAILF("the replica has %lld rows", scalar(r->db, "SELECT count(*) FROM t"));
    if (!same_root(s2, r)) FAIL("the replica's root is not the sequencer's");
    r->l1 = NULL;
    hpplite_node_destroy(r);
    hpplite_node_destroy(s2);
    hpplite_l1_disconnect(l1);
    PASS();
}

static pid_t g_pid;

/* the reference store, as test_store starts it */
static int start_store(const char *dir, int port) {
    char portStr[16], storeDir[600], keyPath[600], script[600];
    FILE *f;
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(storeDir, sizeof storeDir, "%s/pins", DIR);
    snprintf(keyPath, sizeof keyPath, "%s/signer.key", DIR);
    snprintf(script, sizeof script, "%s/hpplite_store.py", dir);
    f = fopen(keyPath, "w");
    if (!f) return -1;
    fputs("0x2020202020202020202020202020202020202020202020202020202020202020\n", f);
    fclose(f);
    g_pid = fork();
    if (g_pid == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", storeDir, "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--price-per-write", "0", "--price-per-mib-month", "0",
               (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[200];
        if (!test_child_alive(g_pid)) { g_pid = 0; return -1; }
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q signer", port);
        if (system(cmd) == 0) return 0;
        usleep(200 * 1000);
    }
    return -1;
}

static void stop_store(void) {
    if (g_pid > 0) { kill(g_pid, SIGTERM); waitpid(g_pid, NULL, 0); g_pid = 0; }
}

/* a snapshot past the lock page: made at the anchor, and a replica starts from it (its image has every page but that
** one, which an image check must count) and reaches the sequencer's root */
static void test_snapshot_past_the_lock_page(const char *storeDir) {
    TEST("a snapshot of a database past the lock-byte page is made at the anchor, and a replica starts from it (#363)");
    if (!storeDir) { printf("SKIP (the store directory is not given) "); PASS(); return; }
    reset_dir();
    int port = test_free_port();
    char url[64];
    if (start_store(storeDir, port) != 0) { stop_store(); FAIL("the store did not start"); }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    static const unsigned char kseq[32] = { [31] = 0x6b };
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "lock_w", .privkey = kseq, .snapshotMs = 3600 * 1000 };
    HppliteNode *w = test_open_node(DIR "/w.db", l1, &o), *sp;
    uint64_t a;
    int i, rows, same;
    sqlite3_int64 snapW, snapR;
    if (!w) { stop_store(); FAIL("open"); }
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_flush_batch(w);              /* batch 1 on its own: a replica uses no snapshot at height 1 */
    for (i = 0; i < 10; i++) hpplite_node_exec(w, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 12) INSERT INTO t(v) SELECT randomblob(3000) FROM c", NULL);
    while (hpplite_node_pending_count(w) > 0) if (hpplite_node_flush_batch(w) == 0) { hpplite_node_destroy(w); stop_store(); FAIL("flush"); }
    a = hpplite_node_anchor(w);
    snapW = status_int(w->db, "snapshot_height");
    hpplite_node_exec(w, "INSERT INTO t(v) VALUES('after')", NULL);
    hpplite_node_flush_batch(w);
    hpplite_node_anchor(w);
    {
        TestNodeOpts so = { .storeUrl = url, .verifyOnly = 1, .fromSnapshot = 1, .nodeId = "lock_s", .privkey = kseq };
        sp = test_open_node(DIR "/s.db", l1, &so);
    }
    rows = sp ? count_rows(sp->db, "t") : -1;
    same = sp ? same_root(w, sp) : 0;
    snapR = sp ? status_int(sp->db, "snapshot_height") : -1;
    printf("(%lld pages, anchor %llu, snapshot %lld; replica rows %d, from %lld, same root %d) ", scalar(w->db, "PRAGMA page_count"), (unsigned long long)a, (long long)snapW, rows, (long long)snapR, same);
    if (sp) { sp->l1 = NULL; hpplite_node_destroy(sp); }
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    stop_store();
    if (a == 0 || snapW != (sqlite3_int64)a) FAIL("no snapshot at the anchor");
    if (snapR != (sqlite3_int64)a) FAIL("the replica did not start from the snapshot");
    if (rows != 121 || !same) FAIL("the replica from the snapshot is not the sequencer's state");
    PASS();
}

int main(int argc, char **argv) {
    /* before any connection: moving it with one open is undefined (sqlite3_test_control) */
    sqlite3_test_control(SQLITE_TESTCTRL_PENDING_BYTE, PENDING);
    printf("=== HPPLite past the lock-byte page ===\n");
    test_suite_init("lockpage", NULL, DIR);
    test_past_the_lock_page();
    test_snapshot_past_the_lock_page(argc > 1 ? argv[1] : NULL);
    remove_dir();
    return test_report();
}
