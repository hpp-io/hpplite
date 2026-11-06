/*
** Crash injection (issue #26): a writer process is killed at deterministic
** points of a flush (HPPLITE_TEST_CRASH_AT) and at random moments while its
** timer thread flushes; a fresh process then reopens the same file against
** the same directory-backed mock L1 and checks the invariants:
**
**   - reopen never errors and refuses nothing;
**   - every captured statement is in L1 or pending, exactly once, after sync;
**   - after a final flush nothing is pending, heights are contiguous, and a
**     reader rebuilt from L1 alone has the same content root as the writer.
**
** Runs as `test_crash` in ctest (a few seconds). ./crash_hpplite --rounds N
** for more random rounds. --l1 dir runs the same against the directory
** backend (l1=file://, #35) instead of the directory-backed mock: there the
** dead writer's lease is expired by the owner's right, not by a mock clock.
**
** --store <pin service dir> (ctest test_crash_store, #167): the writer puts
** its batches at a pin service and anchors nothing; it is killed at random
** between a flush and an anchor; a new sequencer with the key on a fresh file
** recovers the batches from the store by name: nothing up to the last flush
** that returned is lost, it anchors them, a replica from the chain and the
** store equals it.
*/
#include "hpplite.h"
#include "node.h"
#include "batch.h"
#include "capture_rows.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#define DIR "/tmp/hpplite_crash"
#define L1DIR DIR "/l1"

static const unsigned char PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};
static int failures = 0;
static int useDir = 0;                           /* --l1 dir: the directory backend instead of the mock */
static char storeUrl[64];                        /* --store: the pin service of this round (#167) */
#define CHECK(cond, ...) do { if (!(cond)) { printf("    FAIL: " __VA_ARGS__); printf("\n"); failures++; } } while (0)

/* The persistent L1 every process of a round shares */
static HppliteL1 *open_l1(void) {
    return useDir ? hpplite_l1_connect("file://" L1DIR, NULL) : hpplite_l1_create_mock_dir(L1DIR);
}

/* The dead writer's lease: the mock's clock jumps past it; on the directory
** backend the owner (the same key) expires it, as the contract lets the owner */
static void expire_dead_lease(HppliteL1 *l1) {
    if (useDir) {
        hpplite_l1_set_privkey(l1, PRIVKEY);
        char *tx = hpplite_l1_release_lease(l1, NULL);
        sqlite3_free(tx);
    } else {
        hpplite_l1_mock_advance_time(l1, 600);
    }
}

static void reset_dir(void) {
    (void)!system("rm -rf " DIR);
    mkdir(DIR, 0755);
    mkdir(L1DIR, 0755);
}

static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *s; sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &s, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(s) == SQLITE_ROW) v = sqlite3_column_int64(s, 0);
    sqlite3_finalize(s);
    return v;
}

static HppliteNode *open_node(const char *path, HppliteL1 *l1, int intervalMs, size_t limit) {
    HppliteNodeConfig config;
    memset(&config, 0, sizeof(config));
    config.nodeId = "crash";
    memcpy(config.privkey, PRIVKEY, 32);
    config.hasPrivkey = 1;
    config.dataDir = DIR;
    config.dbPath = (char *)path;
    config.checkpointInterval = 2;
    config.batchIntervalMs = intervalMs;
    config.maxBatchBytes = limit;
    if (storeUrl[0]) config.storeUrl = storeUrl;
    HppliteNode *n = hpplite_node_create(&config);
    if (n) {
        hpplite_node_set_l1(n, l1);
        if (hpplite_node_sync_from_l1(n, 1) < 0) { hpplite_node_destroy(n); return NULL; }
        hpplite_node_start(n);
    }
    return n;
}

/* The writer: CREATE once, then rows with unique ids; never closes. */
static void writer(const char *mode, int rows, int intervalMs, size_t limit) {
    HppliteL1 *l1 = open_l1();
    hpplite_l1_mock_set_batch_size_limit(l1, limit);   /* the mock's DA limit; nothing on the directory (the node's own limit splits) */
    HppliteNode *n = open_node(DIR "/w.db", l1, intervalMs, limit);
    if (!n) _exit(2);
    sqlite3_exec(n->db, "CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, v)", NULL, NULL, NULL);
    sqlite3_int64 next = scalar(n->db, "SELECT COALESCE(MAX(id),0)+1 FROM t");
    char sql[256];
    /* page mode (#146): a row nearly a page, 300 random characters in it, so
    ** every ten rows dirty ten pages that deflate past the 1 KB limit of the
    ** series scenario, and a flush is a series of batches there as well */
    const char *pad = n->config.capturePages ? " || hex(randomblob(150)) || printf('%2700c', 'x')" : "";
    for (int i = 0; i < rows; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%lld,'row-%lld-%s'%s)", (long long)(next + i), (long long)(next + i), mode, pad);
        if (i % 7 == 3) {
            sqlite3_exec(n->db, "BEGIN", NULL, NULL, NULL);
            sqlite3_exec(n->db, sql, NULL, NULL, NULL);
            snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%lld,'row-%lld-%s'%s)", (long long)(next + i + 1), (long long)(next + i + 1), mode, pad);
            sqlite3_exec(n->db, sql, NULL, NULL, NULL);
            sqlite3_exec(n->db, "COMMIT", NULL, NULL, NULL);
            i++;
        } else {
            sqlite3_exec(n->db, sql, NULL, NULL, NULL);
        }
        if (!intervalMs && i % 10 == 9) hpplite_node_flush_batch(n);   /* deterministic mode */
        else if (intervalMs) usleep(1000);
    }
    if (!intervalMs) hpplite_node_flush_batch(n);
    _exit(0);   /* deterministic runs die inside a flush above; otherwise plain exit, no close */
}

/* Rows of t; none when the table is not there: the writer was killed before
** its CREATE TABLE committed (an early kill on a slow runner), and nothing
** was captured either (#117). A reader rebuilt from such a log has no table
** too, and counts the same. */
static sqlite3_int64 rows_of_t(sqlite3 *db) {
    if (scalar(db, "SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = 't'") <= 0) return 0;
    return scalar(db, "SELECT count(*) FROM t");
}

/* The checker: reopen, sync, count, flush, rebuild, compare. */
static void check(const char *what) {
    HppliteL1 *l1 = open_l1();
    expire_dead_lease(l1);
    HppliteNode *n = open_node(DIR "/w.db", l1, 0, 0);
    CHECK(n != NULL, "%s: reopen after crash failed", what);
    if (!n) { hpplite_l1_disconnect(l1); return; }
    CHECK(n->state != HPPLITE_STATE_ERROR, "%s: node in error state after reopen", what);

    /* every captured statement exactly once in L1 (after adopt) or pending.
    ** Page mode (#146) has no statements: what a crash loses from the queue
    ** the reopened node ships again as the file it is, and the rebuild below
    ** is the check */
    sqlite3_int64 rows = rows_of_t(n->db);
    uint64_t height = hpplite_node_get_height(n);
    sqlite3_int64 onL1 = 0;
    for (uint64_t h = 1; !n->config.capturePages && h < height; h++) {
        HppliteBatch *b = hpplite_node_fetch_batch(n, h);
        CHECK(b != NULL, "%s: batch %llu missing or unreadable (heights not contiguous)", what, (unsigned long long)h);
        if (!b) break;
        for (int i = 0; i < b->nTxns; i++) {
            if (b->aTxns[i].mode & HPPLITE_MODE_ROW) {
                /* row capture (#19): an insert record of t */
                int op = 0; char *zTab = NULL;
                if (hpplite_rows_entry_info(b->aTxns[i].zSql, &op, &zTab) == SQLITE_OK)
                    onL1 += op == HPPLITE_ROW_INSERT && strcmp(zTab, "t") == 0;
                sqlite3_free(zTab);
            } else {
                onL1 += b->aTxns[i].zSql && strncmp(b->aTxns[i].zSql, "INSERT", 6) == 0;
            }
        }
        hpplite_batch_free(b);
    }
    /* one INSERT per row; the CREATE TABLE IF NOT EXISTS of every restart is
    ** kept too (an autocommit no-op stays logged), so it is not counted */
    if (!n->config.capturePages) {
    sqlite3_int64 pending = scalar(n->db, "SELECT count(*) FROM _hpplite_pending WHERE sql LIKE 'INSERT%'");
    if (!n->config.captureText)
        pending += scalar(n->db, "SELECT count(*) FROM _hpplite_pending WHERE op = 1 AND tbl = 't'");
    CHECK(onL1 + pending == rows, "%s: INSERTs on L1 (%lld) + pending (%lld) != rows (%lld)",
          what, (long long)onL1, (long long)pending, (long long)rows);
    }

    /* a final flush drains pending; a reader rebuilt from L1 matches */
    while (hpplite_node_pending_count(n) > 0) {
        if (hpplite_node_flush_batch(n) == 0) { CHECK(0, "%s: flush after reopen made no progress", what); break; }
    }
    CHECK(hpplite_node_pending_count(n) == 0, "%s: pending after final flush", what);
    unsigned char rootW[32], rootR[32];
    hpplite_node_get_state_root(n, rootW);
    height = hpplite_node_get_height(n);
    hpplite_node_destroy(n);

    HppliteL1 *l1r = open_l1();
    unlink(DIR "/r.db"); unlink(DIR "/r.db-wal"); unlink(DIR "/r.db-shm"); unlink(DIR "/r.db-hpplite.db");   /* the page-mode sidecar goes with the file (#146) */
    HppliteNode *r = open_node(DIR "/r.db", l1r, 0, 0);
    CHECK(r != NULL, "%s: reader open", what);
    if (r) {
        hpplite_node_get_state_root(r, rootR);
        CHECK(hpplite_node_get_height(r) == height, "%s: reader height %llu != %llu", what,
              (unsigned long long)hpplite_node_get_height(r), (unsigned long long)height);
        CHECK(rows_of_t(r->db) == rows, "%s: reader rows %lld != writer rows %lld (writer height %llu, reader height %llu, roots %s)", what, (long long)rows_of_t(r->db), (long long)rows, (unsigned long long)height, (unsigned long long)hpplite_node_get_height(r), memcmp(rootW, rootR, 32) == 0 ? "equal" : "differ");
        CHECK(memcmp(rootW, rootR, 32) == 0, "%s: content root differs after rebuild from L1", what);
        CHECK(r->ctx->nReplaySkipped == 0, "%s: statements skipped on replay", what);
        hpplite_node_destroy(r);          /* releases the lease it claimed on start */
    }
    hpplite_l1_disconnect(l1r);
    hpplite_l1_disconnect(l1);
}

static int run_child(const char *crashAt, int rows, int intervalMs, size_t limit, int killAfterMs) {
    pid_t pid = fork();
    if (pid == 0) {
        if (crashAt) setenv("HPPLITE_TEST_CRASH_AT", crashAt, 1);
        else unsetenv("HPPLITE_TEST_CRASH_AT");
        writer(crashAt ? crashAt : "random", rows, intervalMs, limit);
        _exit(0);
    }
    if (killAfterMs > 0) {
        usleep(killAfterMs * 1000);
        kill(pid, SIGKILL);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* === --store (#167): a sequencer lost between a flush and an anchor === */
#define PINDIR "/tmp/hpplite_crash_pins"
static pid_t pinPid = -1;

static int free_port(void) {
    struct sockaddr_in a;
    socklen_t n = sizeof a;
    int fd = socket(AF_INET, SOCK_STREAM, 0), port = 0;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (fd >= 0 && bind(fd, (struct sockaddr *)&a, sizeof a) == 0 && getsockname(fd, (struct sockaddr *)&a, &n) == 0) port = ntohs(a.sin_port);
    if (fd >= 0) close(fd);
    return port;
}

/* a free store (hpplite-store/2, #214) with a directory of its own for the round: no buckets left by another round */
static int start_pins(const char *svc, int round) {
    char dir[200], cmd[600], script[600], portStr[16];
    int port = free_port();
    snprintf(dir, sizeof dir, PINDIR "/round%d", round);
    snprintf(cmd, sizeof cmd, "mkdir -p %s && echo 0x2020202020202020202020202020202020202020202020202020202020202020 > %s/signer.key", dir, dir);
    if (system(cmd) != 0 || !port) return -1;
    snprintf(script, sizeof script, "%s/hpplite_store.py", svc);
    snprintf(portStr, sizeof portStr, "%d", port);
    pinPid = fork();
    if (pinPid == 0) {
        char key[260];
        int fd = open(PINDIR "/service.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        snprintf(key, sizeof key, "%s/signer.key", dir);
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", dir, "--signer-key", key,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--price-per-write", "0", "--price-per-mib-month", "0",
               (char *)NULL);
        _exit(127);
    }
    snprintf(storeUrl, sizeof storeUrl, "http://127.0.0.1:%d", port);
    for (int i = 0; i < 100; i++) {
        snprintf(cmd, sizeof cmd, "curl -s -m 1 %s/status 2>/dev/null | grep -q signer", storeUrl);
        if (waitpid(pinPid, NULL, WNOHANG) == pinPid) break;       /* the port was taken: never use another's service */
        if (system(cmd) == 0) return 0;
        usleep(100 * 1000);
    }
    return -1;
}

static void stop_pins(void) {
    if (pinPid > 0) { kill(pinPid, SIGTERM); waitpid(pinPid, NULL, 0); }
    pinPid = -1;
}

/* The writer: rows, a flush every five; after each flush that returned, the last id it holds goes to a file (fsync'd) */
static void store_writer(int round) {
    char path[200];
    HppliteL1 *l1 = open_l1();
    snprintf(path, sizeof path, DIR "/sa%d.db", round);
    HppliteNode *n = open_node(path, l1, 0, 0);
    if (!n) _exit(2);
    sqlite3_exec(n->db, "CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, v)", NULL, NULL, NULL);
    for (int i = 1; ; i++) {
        char sql[128];
        snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, 'row %d before the crash')", i, i);
        sqlite3_exec(n->db, sql, NULL, NULL, NULL);
        if (i % 5 == 0 && hpplite_node_flush_batch(n) > 0) {
            char line[32];
            int fd = open(DIR "/flushed", O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { int k = snprintf(line, sizeof line, "%d\n", i); (void)!write(fd, line, (size_t)k); fsync(fd); close(fd); }
        }
    }
}

static void store_check(int round) {
    char path[200], line[32] = "0";
    int fd = open(DIR "/flushed", O_RDONLY), flushed;
    if (fd >= 0) { (void)!read(fd, line, sizeof line - 1); close(fd); }
    flushed = atoi(line);
    HppliteL1 *l1 = open_l1();
    expire_dead_lease(l1);
    snprintf(path, sizeof path, DIR "/sb%d.db", round);              /* the machine is gone: a fresh file */
    HppliteNode *n = open_node(path, l1, 0, 0);
    CHECK(n != NULL, "store round %d: the successor did not open", round);
    if (!n) { hpplite_l1_disconnect(l1); return; }
    sqlite3_int64 rows = rows_of_t(n->db), maxId = rows > 0 ? scalar(n->db, "SELECT max(id) FROM t") : 0;
    printf("(flushed %d, recovered %lld) ", flushed, (long long)rows);
    CHECK(rows >= flushed, "store round %d: %lld rows recovered, %d were flushed before the kill (%s)", round, (long long)rows, flushed, n->ctx->zLastError);
    CHECK(rows == maxId, "store round %d: the recovered ids are not contiguous (%lld rows, max id %lld)", round, (long long)rows, (long long)maxId);
    sqlite3_exec(n->db, "INSERT INTO t VALUES(1000000, 'after the recovery')", NULL, NULL, NULL);
    CHECK(hpplite_node_flush_batch(n) > 0, "store round %d: the successor's own flush", round);
    CHECK(hpplite_node_anchor(n) > 0, "store round %d: the anchor of what was recovered (%s)", round, n->ctx->zLastError);
    unsigned char rootW[32], rootR[32];
    hpplite_node_get_state_root(n, rootW);
    rows = rows_of_t(n->db);
    hpplite_node_destroy(n);
    HppliteL1 *l1r = open_l1();
    snprintf(path, sizeof path, DIR "/sr%d.db", round);
    HppliteNode *r = open_node(path, l1r, 0, 0);
    CHECK(r != NULL, "store round %d: replica open", round);
    if (r) {
        hpplite_node_get_state_root(r, rootR);
        CHECK(rows_of_t(r->db) == rows, "store round %d: replica rows %lld != %lld", round, (long long)rows_of_t(r->db), (long long)rows);
        CHECK(memcmp(rootW, rootR, 32) == 0, "store round %d: replica root differs", round);
        hpplite_node_destroy(r);
    }
    hpplite_l1_disconnect(l1r);
    hpplite_l1_disconnect(l1);
}

static int store_rounds(const char *svc, int rounds) {
    (void)!system("rm -rf " PINDIR);
    mkdir(PINDIR, 0755);
    for (int r = 0; r < rounds; r++) {
        int before = failures, after = 60 + rand() % 400;
        reset_dir();
        if (start_pins(svc, r) != 0) { CHECK(0, "store round %d: the pin service did not start (see " PINDIR "/service.log)", r); stop_pins(); break; }
        printf("  store: kill -9 after %3d ms, before any anchor (round %d) ", after, r + 1);
        pid_t pid = fork();
        if (pid == 0) { store_writer(r); _exit(0); }
        usleep(after * 1000);
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        store_check(r);
        stop_pins();
        printf("%s\n", failures == before ? "ok" : "FAIL");
    }
    if (!failures) (void)!system("rm -rf " PINDIR " " DIR);
    printf("\n=== crash harness (store): %s ===\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    int rounds = 6;
    const char *storeSvc = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--rounds") && i + 1 < argc) rounds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--l1") && i + 1 < argc) useDir = !strcmp(argv[++i], "dir");
        else if (!strcmp(argv[i], "--store") && i + 1 < argc) storeSvc = argv[++i];
    }
    srand((unsigned)getpid());
    if (storeSvc) return store_rounds(storeSvc, rounds);
    printf("L1: %s\n", useDir ? "directory backend (l1=file://)" : "directory-backed mock");

    /* --- deterministic crash points --- */
    static const struct { const char *at; size_t limit; } points[] = {
        { "after_submit",   64 * 1024 },   /* on L1, not finalized locally: must be adopted */
        { "after_finalize", 64 * 1024 },   /* finalized, checkpoint never submitted */
        { "mid_series",     1024 },        /* first chunk of a size-limited series finalized */
    };
    for (size_t p = 0; p < sizeof(points) / sizeof(points[0]); p++) {
        reset_dir();
        printf("  crash at %-15s ", points[p].at);
        int rc = run_child(points[p].at, 40, 0, points[p].limit, 0);
        CHECK(rc == 137, "%s: writer should have died at the crash point (rc=%d)", points[p].at, rc);
        check(points[p].at);
        /* and again on the same file: a second crash on top of the recovered state */
        rc = run_child(points[p].at, 40, 0, points[p].limit, 0);
        CHECK(rc == 137, "%s (2nd): writer should have died (rc=%d)", points[p].at, rc);
        check(points[p].at);
        printf("%s\n", failures ? "FAIL" : "ok");
    }

    /* --- random kill -9 while the timer thread flushes --- */
    reset_dir();
    for (int r = 0; r < rounds; r++) {
        int after = 15 + rand() % 120;
        printf("  kill -9 after %3d ms (round %d) ", after, r + 1);
        int rc = run_child(NULL, 100000, 1, 2048, after);
        CHECK(rc == 137, "random: writer should have been killed (rc=%d)", rc);
        int before = failures;
        check("random");
        printf("%s\n", failures == before ? "ok" : "FAIL");
    }

    (void)!system("rm -rf " DIR);
    printf("\n=== crash harness: %s ===\n", failures ? "FAILED" : "PASSED");
    return failures ? 1 : 0;
}
