/*
** A node's memory against its database's size (#364, #365). SQLite runs a
** database of any size in a few MB of page cache; a node must too. Each
** scenario runs in a child process of its own (this binary run again), which
** prints how far its peak RSS rose over the step measured; the parent holds
** it to a bound far below the data's size.
*/
#include "test_lib.h"
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

#define DIR "/tmp/hpplite_test_memory"
#define DB_MB 64
#define BOUND_MB 24              /* +13 to +16 MB measured (macOS, Linux): the queue, the root, SQLite's cache */
#define BACKLOG_MB 128            /* four times the queue's memory (HPPLITE_QUEUE_MEM, 32 MiB) */
#define BACKLOG_BOUND_MB 48

static long long peak_kb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return (long long)ru.ru_maxrss / 1024;      /* bytes there */
#else
    return (long long)ru.ru_maxrss;             /* KB here */
#endif
}

static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) {
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000 };     /* no timer: nothing posts unless flushed */
    return test_open_node(dbPath, l1, &o);
}

/* rows of 4000 random bytes, plain SQLite (no node): n MB */
static int make_db(const char *zPath, int mb) {
    sqlite3 *db;
    char sql[256];
    if (sqlite3_open(zPath, &db) != SQLITE_OK) return -1;
    sqlite3_exec(db, "PRAGMA journal_mode=WAL; CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL, NULL, NULL);
    snprintf(sql, sizeof sql, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < %d) INSERT INTO t(v) SELECT randomblob(4000) FROM c", mb * 256);
    if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK) { sqlite3_close(db); return -1; }
    sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", NULL, NULL, NULL);
    sqlite3_close(db);
    return 0;
}

/* child: open a node on the DB_MB database and post it (as a new rollup's first batch) */
static int child_open(void) {
    HppliteL1 *l1 = hpplite_l1_connect("file://" DIR "/l1", NULL);     /* the directory chain: posted bytes on disk (the mock keeps every batch in memory) */
    long long before = peak_kb();
    HppliteNode *n = open_node(DIR "/a.db", l1);
    if (!n) return 2;
    if (getenv("MEM_STAGES")) fprintf(stderr, "STAGE open +%lld KB (sqlite %lld KB)\n", peak_kb() - before, (long long)sqlite3_memory_highwater(0) / 1024);
    while (hpplite_node_pending_count(n) > 0) if (hpplite_node_flush_batch(n) == 0) return 3;
    if (getenv("MEM_STAGES")) fprintf(stderr, "STAGE posted +%lld KB (sqlite %lld KB)\n", peak_kb() - before, (long long)sqlite3_memory_highwater(0) / 1024);
    hpplite_node_exec(n, "UPDATE t SET v = randomblob(4000) WHERE id % 50 = 0", NULL);
    if (hpplite_node_flush_batch(n) == 0) return 4;
    printf("RISE_KB %lld\n", peak_kb() - before);
    hpplite_node_destroy(n);
    return 0;
}

/* child: a 4 MB table rewritten 32 times with nothing posted - an outage, by write volume (BACKLOG_MB written) */
static int child_backlog(void) {
    HppliteL1 *l1 = hpplite_l1_connect("file://" DIR "/l1", NULL);     /* the directory chain: posted bytes on disk (the mock keeps every batch in memory) */
    HppliteNode *n = open_node(DIR "/b.db", l1);
    long long before;
    int i;
    if (!n) return 2;
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_exec(n, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 1024) INSERT INTO t(v) SELECT randomblob(4000) FROM c", NULL);
    while (hpplite_node_pending_count(n) > 0) if (hpplite_node_flush_batch(n) == 0) return 3;
    before = peak_kb();
    for (i = 0; i < BACKLOG_MB / 4; i++) {
        int k;
        for (k = 0; k < 16; k++) {          /* 64 rows a commit */
            char sql[160];
            snprintf(sql, sizeof sql, "UPDATE t SET v = randomblob(4000) WHERE id > %d AND id <= %d", k * 64, (k + 1) * 64);
            hpplite_node_exec(n, sql, NULL);
        }
    }
    printf("RISE_KB %lld PENDING %d\n", peak_kb() - before, hpplite_node_pending_count(n));
    while (hpplite_node_pending_count(n) > 0) if (hpplite_node_flush_batch(n) == 0) return 4;
    hpplite_node_destroy(n);
    return 0;
}

/* children: a sequencer posts a SNAP_MB database and stops without anchoring; restarted in a process of its own (a
** peak RSS not raised by the writes), it anchors with a snapshot (store at argv), measured over the anchor alone */
#define SNAP_MB 32
#define SNAP_BOUND_MB 24
static HppliteNode *snap_node(const char *url) {
    static const unsigned char kseq[32] = { [31] = 0x4d };
    HppliteL1 *l1 = hpplite_l1_create_mock_dir(DIR "/l1s");   /* the chain kept between the two processes; it holds only commitments */
    TestNodeOpts o = { .storeUrl = url, .nodeId = "mem_snap", .privkey = kseq, .snapshotMs = 3600 * 1000,
                       .checkpointInterval = TEST_INTERVAL_1000, .noAnchorAtClose = 1 };
    HppliteNode *n = test_open_node(DIR "/s.db", l1, &o);
    if (n) n->ownsL1 = 1;
    return n;
}
static int child_snapsetup(const char *url) {
    HppliteNode *n = snap_node(url);
    int k;
    if (!n) return 2;
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    hpplite_node_flush_batch(n);                        /* batch 1 on its own: no snapshot is used at height 1 */
    for (k = 0; k < SNAP_MB / 4; k++) {
        hpplite_node_exec(n, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c WHERE x < 1024) INSERT INTO t(v) SELECT randomblob(4000) FROM c", NULL);
        while (hpplite_node_pending_count(n) > 0) if (hpplite_node_flush_batch(n) == 0) return 3;
    }
    printf("RISE_KB 0 POSTED %lld\n", (long long)status_int(n->db, "height"));
    hpplite_node_destroy(n);
    return 0;
}
static int child_snapshot(const char *url) {
    HppliteNode *n = snap_node(url);
    long long before, snap, rise;
    uint64_t a;
    if (!n) return 2;
    before = peak_kb();
    a = hpplite_node_anchor(n);
    snap = status_int(n->db, "snapshot_height");
    rise = peak_kb() - before;
    printf("RISE_KB %lld ANCHOR %llu SNAPSHOT %lld\n", rise, (unsigned long long)a, snap);
    hpplite_node_destroy(n);
    return a > 0 && snap == (long long)a ? 0 : 4;
}

/* child: a new replica starts from that snapshot, in a process of its own, measured over its whole start */
static int child_replica(const char *url) {
    static const unsigned char kseq[32] = { [31] = 0x4d };
    HppliteL1 *l1 = hpplite_l1_create_mock_dir(DIR "/l1s");
    TestNodeOpts o = { .storeUrl = url, .nodeId = "mem_snap_r", .privkey = kseq, .verifyOnly = 1, .fromSnapshot = 1,
                       .checkpointInterval = TEST_INTERVAL_1000 };
    long long before = peak_kb(), snap, rows;
    HppliteNode *r = test_open_node(DIR "/r.db", l1, &o);
    if (!r) return 2;
    snap = status_int(r->db, "snapshot_height");
    rows = count_rows(r->db, "t");
    printf("RISE_KB %lld FROM %lld ROWS %lld\n", peak_kb() - before, snap, rows);
    r->ownsL1 = 1;
    hpplite_node_destroy(r);
    return snap > 0 && rows == SNAP_MB * 256 ? 0 : 4;
}

static const char *self;
static pid_t g_store;
static const char *g_url;

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
    g_store = fork();
    if (g_store == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", storeDir, "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--price-per-write", "0", "--price-per-mib-month", "0",
               "--max-bytes", "1073741824", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50; i++) {
        char cmd[200];
        if (!test_child_alive(g_store)) { g_store = 0; return -1; }
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q signer", port);
        if (system(cmd) == 0) return 0;
        usleep(200 * 1000);
    }
    return -1;
}
static void stop_store(void) { if (g_store > 0) { kill(g_store, SIGTERM); waitpid(g_store, NULL, 0); g_store = 0; } }

/* the scenario in a child; its RISE_KB, or -1 */
static long long run_child(const char *which, char *line, size_t cap) {
    int fd[2];
    pid_t pid;
    FILE *f;
    long long rise = -1;
    int status = 0;
    line[0] = 0;
    if (pipe(fd) != 0) return -1;
    pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1); close(fd[0]); close(fd[1]);
        execl(self, self, "child", which, g_url ? g_url : "", (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    f = fdopen(fd[0], "r");
    while (f && fgets(line, (int)cap, f)) if (sscanf(line, "RISE_KB %lld", &rise) == 1) break;
    if (f) { char rest[256]; while (fgets(rest, sizeof rest, f)) {} fclose(f); }
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) { printf("(child exit %d) ", WIFEXITED(status) ? WEXITSTATUS(status) : -1); return -1; }
    return rise;
}

static void test_open_is_not_the_database(void) {
    TEST("opening and posting a 64 MB database raises the node's peak memory by far less than the database (#364)");
    char line[256];
    long long rise;
    if (make_db(DIR "/a.db", DB_MB) != 0) FAIL("make the database");
    rise = run_child("open", line, sizeof line);
    printf("(+%lld MB for a %d MB database) ", rise / 1024, DB_MB);
    if (rise < 0) FAIL("the child");
    if (rise > BOUND_MB * 1024) FAILF("+%lld MB: the node holds the database in memory", rise / 1024);
    PASS();
}

static void test_backlog_is_not_in_memory(void) {
    TEST("128 MB written with nothing posted raises the node's peak memory by its queue's constant, not by what was written (#365)");
    char line[256];
    long long rise = run_child("backlog", line, sizeof line);
    printf("(%s) ", line[0] ? strtok(line, "\n") : "no output");
    if (rise < 0) FAIL("the child");
    if (rise > BACKLOG_BOUND_MB * 1024) FAILF("+%lld MB: the backlog is held in memory", rise / 1024);
    PASS();
}

static void test_snapshot_is_not_the_database(const char *storeDir) {
    TEST("a snapshot of a 32 MB database at an anchor raises the node's peak memory by a chunk, not the database (#364)");
    char line[256], url[64];
    long long rise;
    int port;
    if (!storeDir) { printf("SKIP (the store directory is not given) "); PASS(); return; }
    port = test_free_port();
    if (start_store(storeDir, port) != 0) { stop_store(); FAIL("the store did not start"); }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    g_url = url;
    if (run_child("snapsetup", line, sizeof line) < 0) { g_url = NULL; stop_store(); FAIL("the database not posted"); }
    rise = run_child("snapshot", line, sizeof line);
    printf("(%s) ", line[0] ? strtok(line, "\n") : "no output");
    if (rise < 0) { g_url = NULL; stop_store(); FAIL("the child (no snapshot made?)"); }
    if (rise > SNAP_BOUND_MB * 1024) { g_url = NULL; stop_store(); FAILF("+%lld MB: the snapshot is built in memory", rise / 1024); }
    PASS();
    TEST("a new replica starting from that snapshot raises its peak memory by a chunk, not the database (#364)");
    rise = run_child("replica", line, sizeof line);
    g_url = NULL;
    stop_store();
    printf("(%s) ", line[0] ? strtok(line, "\n") : "no output");
    if (rise < 0) FAIL("the replica (not from the snapshot?)");
    if (rise > SNAP_BOUND_MB * 1024) FAILF("+%lld MB: the snapshot is read into memory", rise / 1024);
    PASS();
}

int main(int argc, char **argv) {
    self = argv[0];
    test_suite_init("memory", NULL, DIR);
    if (argc > 2 && strcmp(argv[1], "child") == 0) {
        if (strcmp(argv[2], "open") == 0) return child_open();
        if (strcmp(argv[2], "backlog") == 0) return child_backlog();
        if (strcmp(argv[2], "snapshot") == 0) return child_snapshot(argc > 3 ? argv[3] : "");
        if (strcmp(argv[2], "snapsetup") == 0) return child_snapsetup(argc > 3 ? argv[3] : "");
        if (strcmp(argv[2], "replica") == 0) return child_replica(argc > 3 ? argv[3] : "");
        return 2;
    }
    printf("=== HPPLite memory against the database's size ===\n");
#if defined(__SANITIZE_ADDRESS__) || (defined(__has_feature) && __has_feature(address_sanitizer))
    /* ASan keeps freed memory in quarantine and maps shadow memory: RSS says nothing of the node's own use */
    printf("SKIP (an AddressSanitizer build: RSS is not the node's)\n");
    return 77;
#endif
    reset_dir();
    test_open_is_not_the_database();
    test_backlog_is_not_in_memory();
    test_snapshot_is_not_the_database(argc > 1 ? argv[1] : NULL);
    if (!getenv("MEM_KEEP")) remove_dir();
    return test_report();
}
