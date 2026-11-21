/*
** A rebuild's time on a long history (#391): a small, busy database - --mb megabytes of rows, two of them
** rewritten in every batch - written through a node into a local reference store on the mock chain: an anchor
** every --anchor batches and a snapshot every --day batches (a batch every 5 s: 360 an anchor of 30 minutes,
** 17280 a day). At each of --marks (heights) the database is rebuilt by a verifier, timed: without a stop - from
** batch 1, or with --keep-days from the oldest snapshot kept - and to stops, each from the newest snapshot at or
** before it: a day's batches less one after it (the worst), half a day's, none. Numbers go to doc/performance.md.
**
**   rebuild_bench --batches 518400 --marks 120960,518400 --store operators/store --dir /tmp/hpplite_rebuildbench
**   rebuild_bench --batches 172800 --marks 172800 --keep-days 7 ...
**
** The mock chain holds 10,000 batches as built for the tests; for more, a build of its own:
**   cmake -S . -B build_bench -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DHPPLITE_MOCK_MAX_BATCHES=600000
**   cmake --build build_bench -j8 --target rebuild_bench
**
** --keep-days: the history behind a snapshot that many days (of --day batches) old is pruned at the store, by the
** wall time the first day took (keep= counts seconds).
*/
#include "test_lib.h"
#include "store.h"
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
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

static const unsigned char kseq[32] = { [31] = 0x34 };

/* one rebuild by a verifier, timed; `to` 0: no stop */
static void rebuild(const char *what, const char *url, HppliteL1 *l1, const char *dir, uint64_t to) {
    static int k;
    char id[32], path[700], cmd[1500];
    TestNodeOpts vo = { .storeUrl = url, .verifyOnly = 1, .nodeId = id, .privkey = kseq, .syncTo = to };
    HppliteNode *v;
    double t0, c0, wall, cpu;
    long long height, snap, mism;
    int saved = dup(2), nul = open("/dev/null", O_WRONLY);
    snprintf(id, sizeof id, "rebuild_v%d", ++k);
    snprintf(path, sizeof path, "%s/%s.db", dir, id);
    fflush(stderr); dup2(nul, 2); close(nul);              /* the verifier's own lines: not the bench's */
    t0 = now_s(); c0 = cpu_s();
    v = test_open_node(path, l1, &vo);
    wall = now_s() - t0; cpu = cpu_s() - c0;
    fflush(stderr); dup2(saved, 2); close(saved);
    if (!v) { printf("  %s: the verifier did not open\n", what); return; }
    height = status_int(v->db, "height") - 1;
    snap = status_int(v->db, "snapshot_height");
    mism = status_int(v->db, "root_mismatches");
    printf("  %s: through batch %lld, begun at %s %lld, %lld batches replayed: %.2f s wall, %.2f s CPU (%.0f batches/s); state %s, root mismatches %lld\n",
           what, height, snap ? "the snapshot at" : "batch", snap ? snap : 1LL, height - snap, wall, cpu, wall > 0 ? (height - snap) / wall : 0,
           status_text(v->db, "state"), mism);
    {
        long nReq = 0, nConn = 0;
        hpplite_store_traffic(v->store, &nReq, &nConn);
        printf("    %ld requests to the store on %ld connections", nReq, nConn);
        if (status_text(v->db, "last_error")[0]) printf("; its error: %s", status_text(v->db, "last_error"));
        printf("\n");
    }
    fflush(stdout);
    v->l1 = NULL;
    hpplite_node_destroy(v);
    snprintf(cmd, sizeof cmd, "rm -f '%s' '%s'-*", path, path);
    (void)!system(cmd);
}

int main(int argc, char **argv) {
    long long batches = 518400, day = 17280, anchorEvery = 360, marks[8], h = 0, snaps[400];
    int mb = 5, nMarks = 0, nSnaps = 0, keepDays = 0, nextMark = 0;
    const char *svc = "operators/store", *dir = "/tmp/hpplite_rebuildbench";
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--batches") && i + 1 < argc) batches = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--day") && i + 1 < argc) day = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--anchor") && i + 1 < argc) anchorEvery = atoll(argv[++i]);
        else if (!strcmp(argv[i], "--mb") && i + 1 < argc) mb = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--keep-days") && i + 1 < argc) keepDays = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--store") && i + 1 < argc) svc = argv[++i];
        else if (!strcmp(argv[i], "--dir") && i + 1 < argc) dir = argv[++i];
        else if (!strcmp(argv[i], "--marks") && i + 1 < argc) {
            char *p = argv[++i];
            while (*p && nMarks < 8) { marks[nMarks++] = strtoll(p, &p, 10); if (*p == ',') p++; }
        }
    }
    if (nMarks == 0) marks[nMarks++] = batches;
    char cmd[700], url[64], path[700], sql[300];
    snprintf(cmd, sizeof cmd, "rm -rf '%s' && mkdir -p '%s'", dir, dir);
    if (system(cmd) != 0) return 1;
    int port = test_free_port();
    pid_t pid = start_store(svc, dir, port);
    if (pid < 0) { fprintf(stderr, "the store did not start\n"); return 1; }
    snprintf(url, sizeof url, "http://127.0.0.1:%d", port);
    HppliteL1 *l1 = hpplite_l1_create_mock();
    /* a snapshot when the bench says: snapshot= is off, and set to 1 ms for the anchor that is to make one */
    TestNodeOpts o = { .storeUrl = url, .nodeId = "rebuild_w", .privkey = kseq, .noAnchorAtClose = 1 };
    snprintf(path, sizeof path, "%s/w.db", dir);
    HppliteNode *w = test_open_node(path, l1, &o);
    if (!w) { fprintf(stderr, "open\n"); kill(pid, SIGTERM); return 1; }
    hpplite_node_exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT)", NULL);
    for (int done = 0; done < mb; done++) {                 /* 1 MB a batch: 1000 rows of 1000 characters */
        hpplite_node_exec(w, "WITH RECURSIVE n(i) AS (SELECT 1 UNION ALL SELECT i + 1 FROM n WHERE i < 1000) INSERT INTO t(v) SELECT hex(randomblob(500)) FROM n", NULL);
        if ((h = hpplite_node_flush_batch(w)) < 1) { fprintf(stderr, "flush: %s\n", w->ctx->zLastError); return 1; }
    }
    snprintf(sql, sizeof sql, "UPDATE t SET v = hex(randomblob(500)) WHERE id IN (abs(random()) %% %d + 1, abs(random()) %% %d + 1)", mb * 1000, mb * 1000);
    printf("a %d MB database, two rows rewritten a batch; an anchor every %lld batches, a snapshot every %lld; %lld batches%s\n", mb, anchorEvery, day, batches,
           keepDays ? "; pruned behind a snapshot --keep-days old" : "; nothing pruned");
    fflush(stdout);
    double t0 = now_s(), tDay = t0;
    int saved = dup(2), nul = open("/dev/null", O_WRONLY);
    fflush(stderr); dup2(nul, 2); close(nul);               /* the node's lines at each snapshot and prune */
    while (h < batches) {
        hpplite_node_exec(w, sql, NULL);
        if ((h = hpplite_node_flush_batch(w)) < 1) { dprintf(saved, "flush: %s\n", w->ctx->zLastError); break; }
        if (h % anchorEvery == 0 || h == batches) {
            int snap = h % day == 0;
            if (snap) w->config.snapshotIntervalMs = 1;
            if ((long long)hpplite_node_anchor(w) != h) { dprintf(saved, "anchor at %lld: %s\n", h, w->ctx->zLastError); break; }
            w->config.snapshotIntervalMs = 0;
            if (snap) {
                if (status_int(w->db, "snapshot_height") != h) { dprintf(saved, "no snapshot at %lld\n", h); break; }
                if (nSnaps < 400) snaps[nSnaps++] = h;
                if (keepDays && nSnaps == 1) {
                    w->config.keepSeconds = (int64_t)((now_s() - tDay) * keepDays) + 1;
                    printf("the first day took %.0f s: keep= %lld s\n", now_s() - tDay, (long long)w->config.keepSeconds);
                }
                printf("day %d: height %lld, %.0f s since the start\n", nSnaps, h, now_s() - t0);
                fflush(stdout);
                sleep(1);                                    /* a snapshot's age counts in whole seconds */
            }
        }
        if (nextMark < nMarks && h == marks[nextMark]) {
            long long last = nSnaps ? snaps[nSnaps - 1] : 0, prev = nSnaps > 1 ? snaps[nSnaps - 2] : 0;
            char what[160];
            fflush(stderr); dup2(saved, 2);
            printf("at height %lld (%.1f days), written in %.0f s; store: ", h, (double)h / day, now_s() - t0);
            fflush(stdout);
            snprintf(cmd, sizeof cmd, "du -sh '%s/pins' | cut -f1 | tr -d '\\n'; printf ' in '; find '%s/pins' -type f | wc -l | tr -d ' \\n'; echo ' files'", dir, dir);
            (void)!system(cmd);
            rebuild(keepDays ? "no stop (the oldest snapshot kept, or batch 1)" : "no stop (from batch 1)", url, l1, dir, 0);
            if (last && prev) {
                snprintf(what, sizeof what, "--to %lld (one less than a snapshot's height: a day's batches less one)", last - 1);
                rebuild(what, url, l1, dir, (uint64_t)(last - 1));
                snprintf(what, sizeof what, "--to %lld (half a day past a snapshot)", prev + day / 2);
                rebuild(what, url, l1, dir, (uint64_t)(prev + day / 2));
                snprintf(what, sizeof what, "--to %lld (a snapshot's height)", prev);
                rebuild(what, url, l1, dir, (uint64_t)prev);
            }
            nul = open("/dev/null", O_WRONLY);
            fflush(stderr); dup2(nul, 2); close(nul);
            nextMark++;
        }
    }
    fflush(stderr); dup2(saved, 2); close(saved);
    w->ownsL1 = 0;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    kill(pid, SIGTERM); waitpid(pid, NULL, 0);
    return 0;
}
