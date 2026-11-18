/*
** A snapshot's time and memory (#233): a database of --mb megabytes of random rows (incompressible: deflate's worst
** case), written through a node into a local reference store, then one anchor with snapshot= on - the snapshot is
** built at it. Prints the anchor's wall time and the process's peak RSS before and after it; then a replica started
** from that snapshot, its time and its peak. Numbers go to doc/performance.md.
**
**   snapshot_bench --mb 200 --store operators/store [--dir /tmp/hpplite_snapbench]
*/
#include "test_lib.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }

static double peak_mb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1048576.0;                       /* bytes on macOS */
#else
    return ru.ru_maxrss / 1024.0;                          /* kilobytes on Linux */
#endif
}

/* the process's CPU seconds: the node's own work, apart from the wall time spent waiting on the store (the reference
** store hashes each put in pure Python, about 2 s a MB) */
static double cpu_s(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}

static pid_t start_store(const char *svc, const char *dir, int port) {
    char script[600], portStr[16], keyPath[600], storeDir[600];
    FILE *f;
    pid_t pid;
    snprintf(script, sizeof script, "%s/hpplite_store.py", svc);
    snprintf(portStr, sizeof portStr, "%d", port);
    snprintf(keyPath, sizeof keyPath, "%s/signer.key", dir);
    snprintf(storeDir, sizeof storeDir, "%s/pins", dir);
    if ((f = fopen(keyPath, "w"))) { fputs("0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d\n", f); fclose(f); }
    if ((pid = fork()) == 0) {
        char logPath[640];
        snprintf(logPath, sizeof logPath, "%s/store.log", dir);       /* each request, timed by its log line */
        int fd = open(logPath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", storeDir, "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--price-per-write", "0", "--price-per-mib-month", "0",
               "--max-bytes", "1073741824", "--verbose", (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 100; i++) {
        char cmd[200];
        snprintf(cmd, sizeof cmd, "curl -s -m 2 http://127.0.0.1:%d/status 2>/dev/null | grep -q signer", port);
        if (system(cmd) == 0) return pid;
        usleep(200 * 1000);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    return -1;
}

int main(int argc, char **argv) {
    int mb = 200;
    const char *svc = "operators/store", *dir = "/tmp/hpplite_snapbench";
    static const unsigned char kseq[32] = { [31] = 0x33 };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--store") && i + 1 < argc) svc = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
    }
    char cmd[700], url[64], path[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) return 1;
    int port = test_free_port();
    pid_t pid = start_store(svc, dir, port);
    if (pid < 0) { fprintf(stderr, "the store did not start\n"); return 1; }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "snapbench_w", .privkey = kseq, .snapshotMs = 3600 * 1000, .noAnchorAtClose = 1 };
    snprintf(path, sizeof path, "%s/w.db", dir);
    HppliteNode *w = test_open_node(path, l1, &o);
    if (!w) { fprintf(stderr, "open\n"); kill(pid, SIGTERM); return 1; }
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v BLOB)", NULL);
    double t0 = now_s();
    for (int done = 0; done < mb; done += 4) {             /* 4 MB a commit and a batch: 1024 rows of 4 KB */
        hpplite_node_exec(w, "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1024) INSERT INTO t(v) SELECT randomblob(4000) FROM n", NULL);
        if (hpplite_node_flush_batch(w) < 1) { fprintf(stderr, "flush: %s\n", w->ctx->zLastError); break; }
    }
    long long pages = 0;
    { sqlite3_stmt *s; if (sqlite3_prepare_v2(w->db, "PRAGMA page_count", -1, &s, NULL) == SQLITE_OK) { if (sqlite3_step(s) == SQLITE_ROW) pages = sqlite3_column_int64(s, 0); sqlite3_finalize(s); } }
    double written = now_s() - t0, before = peak_mb();
    printf("database: %lld pages (%.0f MB), written in %.1f s; peak RSS before the anchor %.0f MB\n", pages, pages * 4096 / 1048576.0, written, before);
    fflush(stdout);
    t0 = now_s();
    double c0 = cpu_s();
    uint64_t h = hpplite_node_anchor(w);
    double took = now_s() - t0, cpu = cpu_s() - c0, after = peak_mb();
    printf("anchor with the snapshot: height %llu, snapshot_height %lld, %.1f s wall, %.1f s CPU (the node's own work); peak RSS after %.0f MB (+%.0f MB)\n",
           (unsigned long long)h, (long long)scalar(w->db, "SELECT snapshot_height FROM hpplite_status"), took, cpu, after, after - before);
    fflush(stdout);
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    return 0;
}
