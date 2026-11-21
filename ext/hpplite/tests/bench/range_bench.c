/*
** A range of a day's batches (#394). anchor=24h on a database written round the clock at interval=5s puts 17,280
** batches in one range: one manifest at the store, one commitment on the chain. A small, busy database - --mb
** megabytes of rows, two of them rewritten in every batch - written through a node into a local reference store on
** the mock chain, --range batches between anchors:
**
**   day 1  the batches written and none anchored, then the anchor: its time (the flush waits for it: the timer
**          takes the same lock for both), the manifest's bytes (the canonical store takes 16 MiB an object), the
**          process's peak memory; a verifier reads the range from batch 1.
**   day 2  the batches written and none anchored, and the host lost: a new one on a fresh file with the same key
**          reads them from the store by name (#167), timed; its anchor makes a snapshot.
**   then   a few batches and an anchor that prunes the history behind that snapshot (keep=), timed, with the
**          files the store holds before and after.
**
** Numbers go to doc/performance.md. Time is compressed - a day's batches in minutes - so what goes by the clock is
** not shown: the lease's renewals over a day, the store's hourly tick.
**
** The mock chain holds 10,000 batches as built for the tests; for more, a build of its own:
**   cmake -S . -B build_bench -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DHPPLITE_MOCK_MAX_BATCHES=600000
**   cmake --build build_bench -j8 --target range_bench
**   build_bench/range_bench --range 17280 --store operators/store --dir /tmp/hpplite_rangebench
*/
#include "test_lib.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double cpu_s(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_utime.tv_sec + ru.ru_utime.tv_usec / 1e6 + ru.ru_stime.tv_sec + ru.ru_stime.tv_usec / 1e6;
}
static double peak_mb(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
#ifdef __APPLE__
    return ru.ru_maxrss / 1048576.0;            /* bytes there */
#else
    return ru.ru_maxrss / 1024.0;               /* KB here */
#endif
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
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); }
        execlp("python3", "python3", "-I", script, "--port", portStr, "--dir", storeDir, "--signer-key", keyPath,
               "--pay-to", "0x1111111111111111111111111111111111111111", "--price-per-write", "0", "--price-per-mib-month", "0",
               "--max-bytes", "1073741824", (char *)NULL);
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

static const unsigned char kseq[32] = { [31] = 0x35 };
static int savedErr = -1;

/* the node's own lines (a snapshot, a prune, a recovery) are not the bench's */
static void quiet(int on) {
    fflush(stderr);
    if (on && savedErr < 0) { int nul = open("/dev/null", O_WRONLY); savedErr = dup(2); dup2(nul, 2); close(nul); }
    else if (!on && savedErr >= 0) { dup2(savedErr, 2); close(savedErr); savedErr = -1; }
}

/* a number a shell command prints */
static long long sh_ll(const char *fmt, const char *dir) {
    char cmd[1600], out[64] = "";
    FILE *p;
    snprintf(cmd, sizeof cmd, fmt, dir, dir);
    if ((p = popen(cmd, "r"))) { if (!fgets(out, sizeof out, p)) out[0] = 0; pclose(p); }
    return atoll(out);
}
static long long store_files(const char *dir) { return sh_ll("find '%s/pins' -type f | wc -l", dir); }
static long long names_held(const char *dir) { return sh_ll("find '%s/pins/b' -path '*/n/*' -type f | wc -l", dir); }

/* `n` batches of two rewritten rows, none anchored; the height reached, 0 on a failure */
static long long write_batches(HppliteNode *w, const char *sql, long long n) {
    long long h = 0;
    for (long long i = 0; i < n; i++) {
        hpplite_node_exec(w, sql, NULL);
        if ((h = hpplite_node_flush_batch(w)) < 1) { quiet(0); fprintf(stderr, "flush: %s\n", w->ctx->zLastError); return 0; }
    }
    return h;
}

/* one anchor, timed; the largest object the store took for it (the manifest, or a snapshot's chunk) */
static int anchor_timed(HppliteNode *w, const char *what, const char *dir, long long want) {
    char cmd[700];
    double t0, c0, p0 = peak_mb();
    long long got, files = store_files(dir), names = names_held(dir);
    snprintf(cmd, sizeof cmd, "touch '%s/marker'", dir);
    (void)!system(cmd);
    usleep(1100 * 1000);                                    /* -newer compares whole seconds on some file systems */
    t0 = now_s(); c0 = cpu_s();
    got = (long long)hpplite_node_anchor(w);
    printf("%s: %.2f s wall, %.2f s CPU; the largest object put: %lld bytes; peak memory %.0f MB (%+.0f MB over the anchor); "
           "the store's files %lld -> %lld, names %lld -> %lld\n", what, now_s() - t0, cpu_s() - c0,
           sh_ll("find '%s/pins/c' -type f -newer '%s/marker' ! -name '*.refs' -exec ls -l {} + 2>/dev/null | awk '{print $5}' | sort -n | tail -1", dir),
           peak_mb(), peak_mb() - p0, files, store_files(dir), names, names_held(dir));
    fflush(stdout);
    if (got != want) { quiet(0); fprintf(stderr, "anchor: %lld, not %lld: %s\n", got, want, w->ctx->zLastError); return -1; }
    return 0;
}

/* one rebuild by a verifier without a stop, timed */
static void rebuild(const char *what, const char *url, HppliteL1 *l1, const char *dir) {
    static int k;
    char id[32], path[700], cmd[1500];
    TestNodeOpts vo = { .storeUrl = url, .verifyOnly = 1, .nodeId = id, .privkey = kseq };
    HppliteNode *v;
    double t0, c0, wall, cpu, p0 = peak_mb();
    long long height, snap;
    snprintf(id, sizeof id, "range_v%d", ++k);
    snprintf(path, sizeof path, "%s/%s.db", dir, id);
    t0 = now_s(); c0 = cpu_s();
    v = test_open_node(path, l1, &vo);
    wall = now_s() - t0; cpu = cpu_s() - c0;
    if (!v) { printf("%s: the verifier did not open\n", what); return; }
    height = status_int(v->db, "height") - 1;
    snap = status_int(v->db, "snapshot_height");
    printf("%s: through batch %lld, begun at %s %lld: %.2f s wall, %.2f s CPU (%.0f batches/s); state %s, root mismatches %lld; peak memory %+.0f MB\n",
           what, height, snap > 0 ? "the snapshot at" : "batch", snap > 0 ? snap : 1LL, wall, cpu, wall > 0 ? (height - (snap > 0 ? snap : 0)) / wall : 0,
           status_text(v->db, "state"), (long long)status_int(v->db, "root_mismatches"), peak_mb() - p0);
    fflush(stdout);
    v->l1 = NULL;
    hpplite_node_destroy(v);
    snprintf(cmd, sizeof cmd, "rm -f '%s' '%s'-*", path, path);
    (void)!system(cmd);
}

int main(int argc, char **argv) {
    long long range = 17280, h, base;
    int mb = 5;
    const char *svc = "operators/store", *dir = "/tmp/hpplite_rangebench";
    setvbuf(stdout, NULL, _IOLBF, 0);                             /* a line as it is printed, also into a file */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--range") && i + 1 < argc) range = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--store") && i + 1 < argc) svc = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
    }
    char cmd[700], url[64], path[700], sql[300];
    struct stat sb;
    double t0;
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) return 1;
    int port = test_free_port();
    pid_t pid = start_store(svc, dir, port);
    if (pid < 0) { fprintf(stderr, "the store did not start\n"); return 1; }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .storeUrl = url, .nodeId = "range_w", .privkey = kseq, .noAnchorAtClose = 1 };
    snprintf(path, sizeof path, "%s/w.db", dir);
    HppliteNode *w = test_open_node(path, l1, &o), *w2 = NULL;
    if (!w) { fprintf(stderr, "open\n"); kill(pid, SIGTERM); return 1; }
    quiet(1);
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    for (int done = 0; done < mb; done++) {                 /* 1 MB a batch: 1000 rows of 1000 characters */
        hpplite_node_exec(w, "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1000) INSERT INTO t(v) SELECT hex(randomblob(500)) FROM n", NULL);
        if (hpplite_node_flush_batch(w) < 1) { quiet(0); fprintf(stderr, "flush: %s\n", w->ctx->zLastError); goto out; }
    }
    base = (long long)hpplite_node_anchor(w);               /* the database itself is anchored: the ranges after it are the days */
    if (base < 1) { quiet(0); fprintf(stderr, "anchor: %s\n", w->ctx->zLastError); goto out; }
    snprintf(sql, sizeof sql, "UPDATE t SET v = hex(randomblob(500)) WHERE id IN (abs(random()) %% %d + 1, abs(random()) %% %d + 1)", mb * 1000, mb * 1000);
    printf("a %d MB database anchored at batch %lld, two rows rewritten a batch; %lld batches a range\n", mb, base, range);

    /* day 1 */
    t0 = now_s();
    if (!(h = write_batches(w, sql, range))) goto out;
    snprintf(cmd, sizeof cmd, "%s-hpplite.db", path);
    printf("day 1: batches %lld to %lld written in %.0f s (%.0f a second), none anchored: %lld rows wait for the anchor, the bookkeeping file %.1f MB, peak memory %.0f MB\n",
           base + 1, h, now_s() - t0, range / (now_s() - t0), (long long)scalar(w->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored"),
           stat(cmd, &sb) == 0 ? sb.st_size / 1048576.0 : -1.0, peak_mb());
    if (anchor_timed(w, "  the anchor of the range", dir, h) != 0) goto out;
    rebuild("  a verifier, no stop", url, l1, dir);

    /* day 2: written, none anchored, the host lost */
    t0 = now_s();
    if (!(h = write_batches(w, sql, range))) goto out;
    printf("day 2: batches to %lld written in %.0f s, none anchored; the host is lost\n", h, now_s() - t0);
    hpplite_node_stop_timer(w);
    w->ownsL1 = 0; w->l1 = NULL;
    hpplite_node_destroy(w); w = NULL;
    hpplite_l1_mock_advance_time(l1, 400);                  /* the lost one's lease runs out */
    {
        TestNodeOpts o2 = { .storeUrl = url, .nodeId = "range_w2", .privkey = kseq, .noAnchorAtClose = 1 };
        double c0 = cpu_s(), p0 = peak_mb();
        snprintf(path, sizeof path, "%s/w2.db", dir);
        t0 = now_s();
        w2 = test_open_node(path, l1, &o2);
        if (!w2) { quiet(0); fprintf(stderr, "the new host did not open\n"); goto out; }
        printf("  a new host on a fresh file with the same key: height %lld, %lld batches read from the store by name and kept for the anchor: %.2f s wall, %.2f s CPU (%.0f names a second); peak memory %+.0f MB\n",
               (long long)status_int(w2->db, "height") - 1, (long long)scalar(w2->ctx->bkDb, "SELECT count(*) FROM _hpplite_unanchored"),
               now_s() - t0, cpu_s() - c0, range / (now_s() - t0), peak_mb() - p0);
        fflush(stdout);
    }
    if (status_int(w2->db, "height") - 1 != h) { quiet(0); fprintf(stderr, "the new host is at %lld, not %lld: %s\n", (long long)status_int(w2->db, "height") - 1, h, status_text(w2->db, "last_error")); goto out; }
    w2->config.snapshotIntervalMs = 1;
    if (anchor_timed(w2, "  its anchor of the range, with a snapshot", dir, h) != 0) goto out;
    printf("  the snapshot is at %lld\n", (long long)status_int(w2->db, "snapshot_height"));

    /* the prune behind that snapshot */
    sleep(2);                                               /* a snapshot's age counts in whole seconds */
    if (!(h = write_batches(w2, sql, 3))) goto out;
    w2->config.keepSeconds = 1;
    w2->config.snapshotIntervalMs = 0;                      /* the prune alone: no second snapshot in this anchor's time */
    if (anchor_timed(w2, "three batches later, the anchor that prunes behind that snapshot (keep=)", dir, h) != 0) goto out;
    rebuild("  a verifier, no stop", url, l1, dir);
out:
    quiet(0);
    if (w) { w->ownsL1 = 0; hpplite_node_destroy(w); }
    if (w2) { w2->ownsL1 = 0; hpplite_node_destroy(w2); }
    hpplite_l1_disconnect(l1);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    return 0;
}
