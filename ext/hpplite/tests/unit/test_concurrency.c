/*
** HPPLite with several writers on one file at once: threads with their own
** connections (a pool), and processes (php-fpm, gunicorn workers, a cron job
** beside a server). SQLite serializes their commits; the log must hold their
** statements in that same order, or a replica replaying it diverges (two
** INSERTs with automatic ids, swapped, give swapped rows). The invariant
** every test ends with: a replica rebuilt from L1 alone equals the writer,
** row for row.
*/

#include "test_lib.h"
#include "batch.h"
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

static const unsigned char PRIVKEY[32] = {
    0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,0xb0,
    0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0
};
#define DIR "/tmp/hpplite_test_concurrency"
#define N 1500

static const char *self_path;
/* A writer process: this binary run again, so nothing of the parent's SQLite
** (open files, lock bookkeeping, HPPLite's in-process registry) is inherited -
** SQLite must not be carried across fork() */
static pid_t spawn_writer(const char *mode, const char *db, const char *who, int n) {
    pid_t pid = fork();
    if (pid == 0) {
        char ns[16]; snprintf(ns, sizeof(ns), "%d", n);
        execl(self_path, self_path, mode, db, who, ns, (char *)NULL);
        _exit(127);
    }
    return pid;
}

/* the mock's DA limit is lifted: these tests are about order, not size */
static const TestNodeOpts NODE = { .daLimit = 64 * 1024 * 1024, .checkpointInterval = TEST_INTERVAL_1000 };
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return test_open_node(dbPath, l1, &NODE); }

/* A full node - L1 configured, as an application's connection is - on the
** file-backed mock, which every process sees as one chain */
#define L1DIR DIR "/l1"
static HppliteNode *open_shared(const char *dbPath, HppliteL1 **pl1) {
    *pl1 = hpplite_l1_create_mock_dir(L1DIR);
    return open_node(dbPath, *pl1);
}
static void hex32(const unsigned char *p, char *out) { for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", p[i]); }
static int wait_file(const char *path, int ms) {
    for (int i = 0; i < ms / 10; i++) { if (access(path, F_OK) == 0) return 1; usleep(10000); }
    return 0;
}
static int lease_holder(HppliteL1 *l1, unsigned char out[32]) {
    HppliteL1State *st = hpplite_l1_get_state(l1);
    int held = 0;
    if (!st) return -1;
    uint64_t now = st->currentTimestamp ? st->currentTimestamp : (uint64_t)time(NULL);
    held = st->leaseExpiry > now;
    if (held) memcpy(out, st->sequencerInstance, 32);
    hpplite_l1_state_free(st);
    return held;
}

/* the replica check: rebuild from L1 into an empty file, compare root and
** every row of every table (the caller flushed; the writer stays open) */
static const char *replica_differs(HppliteNode *w, HppliteL1 *l1, const char *path) {
    return test_replica_check(w, l1, path, &NODE, 0, 0, 0);
}

typedef struct { sqlite3 *db; const char *who; int n; int failed; } Writer;
static void *write_autocommit(void *arg) {
    Writer *w = arg;
    for (int i = 0; i < w->n; i++) {
        char sql[96];
        snprintf(sql, sizeof(sql), "INSERT INTO t(who, k) VALUES('%s', %d)", w->who, i);
        char *err = NULL;
        if (sqlite3_exec(w->db, sql, NULL, NULL, &err) != SQLITE_OK) {
            if (getenv("CONC_VERBOSE") && w->failed < 3) fprintf(stderr, "[%s] %s: %s\n", w->who, sql, err);
            w->failed++;
        }
        sqlite3_free(err);
    }
    return NULL;
}
/* What a pool of request handlers does to one table: autocommit inserts,
** short transactions, no-op updates, upserts on a shared key, deletes,
** counters - every shape whose replay depends on order */
static void *write_mixed(void *arg) {
    Writer *w = arg;
    int effectsOnly = getenv("CONC_EFFECTS_ONLY") != NULL;   /* diagnosis: no statement that can be a no-op */
    for (int i = 0; i < w->n; i++) {
        char sql[256];
        if (effectsOnly && (i % 6 == 2 || i % 6 == 4 || i % 6 == 5)) continue;
        switch (i % 6) {
        case 0: snprintf(sql, sizeof(sql), "INSERT INTO t(who, k) VALUES('%s', %d)", w->who, i); break;
        case 1: snprintf(sql, sizeof(sql), "BEGIN; INSERT INTO t(who, k) VALUES('%s', %d); "
                         "UPDATE c SET v = v + 1 WHERE id = 1; INSERT INTO s(who) VALUES('%s'); COMMIT", w->who, i, w->who); break;
        case 2: snprintf(sql, sizeof(sql), "UPDATE t SET k = k WHERE id < 0"); break;   /* no-op: logged, then unlogged */
        case 3: snprintf(sql, sizeof(sql), "INSERT INTO u(key, v, who) VALUES(%d, 1, '%s') "
                         "ON CONFLICT(key) DO UPDATE SET v = v + 1, who = excluded.who", i % 17, w->who); break;
        case 4: snprintf(sql, sizeof(sql), "DELETE FROM t WHERE id = (SELECT max(id) FROM t WHERE who = '%s')", w->who); break;
        default: snprintf(sql, sizeof(sql), "INSERT OR IGNORE INTO u(key, v, who) VALUES(%d, 100, '%s')", i % 23, w->who); break;
        }
        char *err = NULL;
        if (sqlite3_exec(w->db, sql, NULL, NULL, &err) != SQLITE_OK) {
            if (getenv("CONC_VERBOSE") && w->failed < 3) fprintf(stderr, "[%s] %.80s: %s\n", w->who, sql, err);
            sqlite3_free(err);
            w->failed++;
            sqlite3_exec(w->db, "ROLLBACK", NULL, NULL, NULL);
        }
    }
    return NULL;
}
#define SCHEMA "CREATE TABLE t(id INTEGER PRIMARY KEY, who, k); " \
    "CREATE TABLE s(id INTEGER PRIMARY KEY AUTOINCREMENT, who); " \
    "CREATE TABLE u(key INTEGER PRIMARY KEY, v, who); CREATE TABLE c(id INTEGER PRIMARY KEY, v); INSERT INTO c VALUES(1, 0)"

typedef struct { HppliteNode *n; volatile int stop; int flushes; } Flusher;
static void *flusher(void *arg) {
    Flusher *f = arg;
    while (!f->stop) { hpplite_node_flush_batch(f->n); f->flushes++; usleep(200); }
    return NULL;
}

static void test_two_connections_two_threads(void) {
    TEST("two connections on two threads, interleaved autocommit INSERTs: the replica equals the writer");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/pool.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, who, k)", NULL);
    HppliteNode *b = open_node(DIR "/pool.db", l1);
    if (!b || !b->follower) FAIL("b should be a follower");
    Writer wa = { a->db, "a", N, 0 }, wb = { b->db, "b", N, 0 };
    Flusher f = { a, 0, 0 };
    pthread_t ta, tb, tf;
    pthread_create(&tf, NULL, flusher, &f);
    pthread_create(&ta, NULL, write_autocommit, &wa);
    pthread_create(&tb, NULL, write_autocommit, &wb);
    pthread_join(ta, NULL); pthread_join(tb, NULL);
    f.stop = 1; pthread_join(tf, NULL);
    hpplite_node_destroy(b);
    while (hpplite_node_pending_count(a) > 0 && hpplite_node_flush_batch(a) > 0) {}
    const char *why = replica_differs(a, l1, DIR "/pool_replica.db");
    int left = hpplite_node_pending_count(a);
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l1);
    printf("(%d flushes, %d/%d failed) ", f.flushes, wa.failed, wb.failed);
    if (wa.failed || wb.failed) FAIL("writes failed");
    if (left) FAIL("pending left");
    if (why) FAIL(why);
    PASS();
}

/* A second process writing the same file: it captures into the shared log
** and posts nothing (no L1 of its own - what a follower process does) */
static void test_two_processes(void) {
    TEST("a second process writing the same file at once: the replica equals the writer");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/proc.db", l1);
    if (!a) FAIL("open a");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, who, k)", NULL);
    hpplite_node_flush_batch(a);
    pid_t pid = spawn_writer("--autocommit", DIR "/proc.db", "c", N);
    Writer wa = { a->db, "a", N, 0 };
    Flusher f = { a, 0, 0 };
    pthread_t tf;
    pthread_create(&tf, NULL, flusher, &f);
    write_autocommit(&wa);
    int status = 0;
    waitpid(pid, &status, 0);
    f.stop = 1; pthread_join(tf, NULL);
    while (hpplite_node_pending_count(a) > 0 && hpplite_node_flush_batch(a) > 0) {}
    const char *why = replica_differs(a, l1, DIR "/proc_replica.db");
    int left = hpplite_node_pending_count(a);
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l1);
    printf("(%d flushes) ", f.flushes);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) FAIL("child writes failed");
    if (wa.failed) FAIL("parent writes failed");
    if (left) FAIL("pending left");
    if (why) FAIL(why);
    PASS();
}

/* Four threads on four connections and three processes, all writing the
** mixed workload at once while the sequencer flushes continuously */
#define THREADS 4
#define PROCS 3
#define MIXED 1200
static void test_mixed_threads_and_processes(void) {
    TEST("4 threads and 3 processes, mixed workload at once, flushing throughout: the replica equals the writer");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/mixed.db", l1);
    if (!a) FAIL("open");
    if (hpplite_node_exec(a, SCHEMA, NULL) != 0) FAIL("schema");
    hpplite_node_flush_batch(a);
    pid_t pids[PROCS];
    int nprocs = getenv("CONC_PROCS") ? atoi(getenv("CONC_PROCS")) : PROCS;
    int nthreads = getenv("CONC_THREADS") ? atoi(getenv("CONC_THREADS")) : THREADS;
    for (int p = 0; p < nprocs; p++) {
        char who[8]; snprintf(who, sizeof(who), "p%d", p);
        pids[p] = spawn_writer("--mixed", DIR "/mixed.db", who, MIXED);
    }
    HppliteNode *conn[THREADS]; Writer w[THREADS]; pthread_t th[THREADS];
    char names[THREADS][8];
    for (int i = 0; i < nthreads; i++) {
        conn[i] = i == 0 ? a : open_node(DIR "/mixed.db", l1);
        if (!conn[i]) FAIL("open thread connection");
        sqlite3_busy_timeout(conn[i]->db, 20000);
        snprintf(names[i], sizeof(names[i]), "t%d", i);
        w[i] = (Writer){ conn[i]->db, names[i], MIXED, 0 };
    }
    Flusher f = { a, 0, 0 };
    pthread_t tf;
    pthread_create(&tf, NULL, flusher, &f);
    for (int i = 1; i < nthreads; i++) pthread_create(&th[i], NULL, write_mixed, &w[i]);
    if (nthreads > 0) write_mixed(&w[0]);
    int failed = 0, childFailed = 0;
    for (int i = 0; i < nthreads; i++) { if (i > 0) pthread_join(th[i], NULL); failed += w[i].failed; }
    for (int p = 0; p < nprocs; p++) {
        int status = 0; waitpid(pids[p], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            /* say how it ended: 2 = could not open (the write lock past its
            ** timeout under load), 1 = statements failed, a signal = crashed (#94) */
            childFailed++;
            if (WIFEXITED(status)) printf("[process %d exited %d] ", p, WEXITSTATUS(status));
            else printf("[process %d killed by signal %d] ", p, WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        }
    }
    f.stop = 1; pthread_join(tf, NULL);
    for (int i = 1; i < nthreads; i++) hpplite_node_destroy(conn[i]);
    while (hpplite_node_pending_count(a) > 0 && hpplite_node_flush_batch(a) > 0) {}
    const char *why = replica_differs(a, l1, DIR "/mixed_replica.db");
    int left = hpplite_node_pending_count(a);
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l1);
    printf("(%d flushes) ", f.flushes);
    /* everything that went wrong, not the first thing (#94) */
    if (failed || childFailed || left || why)
        FAILF("%d thread statements failed, %d processes had failures, %d pending left%s%s",
              failed, childFailed, left, why ? ", " : "", why ? why : "");
    PASS();
}

/* --hold <db> <dir> <n>: a full node that writes n rows, says who it is in
** <dir>/ready (sequencer flag, instance id), and closes when <dir>/stop appears */
static int holder_main(char **argv) {
    HppliteL1 *l1;
    char path[512], id[65];
    HppliteNode *n = open_shared(argv[2], &l1);
    if (!n) return 1;
    hpplite_node_exec(n, "CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, who, k)", NULL);
    Writer w = { n->db, "h", atoi(argv[4]), 0 };
    write_autocommit(&w);
    hex32(n->instanceId, id);
    snprintf(path, sizeof(path), "%s/ready.tmp", argv[3]);
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d %s\n", n->follower ? 0 : 1, id); fclose(f); }
    char ready[512]; snprintf(ready, sizeof(ready), "%s/ready", argv[3]);
    rename(path, ready);
    snprintf(path, sizeof(path), "%s/stop", argv[3]);
    wait_file(path, 60000);
    hpplite_node_destroy(n);
    hpplite_l1_disconnect(l1);
    return w.failed ? 1 : 0;
}

/* a writer process (spawn_writer): captures into the shared log, no L1 of its own */
static int writer_main(char **argv) {
    HppliteNode *c = NULL;
    /* the open needs the write lock too, under the node's 5 s busy timeout:
    ** under load (ctest -j4, the parent's flusher, the other writers) one
    ** contender can starve past it - the open is tried again, as the
    ** statements get a longer timeout below (#94) */
    for (int k = 0; k < 5 && !c; k++) c = open_node(argv[2], NULL);
    if (!c) { fprintf(stderr, "writer %s: could not open the database (the write lock, five times)\n", argv[3]); return 2; }
    sqlite3_busy_timeout(c->db, 20000);
    Writer w = { c->db, argv[3], atoi(argv[4]), 0 };
    if (strcmp(argv[1], "--mixed") == 0) write_mixed(&w); else write_autocommit(&w);
    hpplite_node_destroy(c);
    if (w.failed) fprintf(stderr, "writer %s: %d statements failed\n", argv[3], w.failed);
    return w.failed ? 1 : 0;
}

/* The election across processes: the first full node on a file sequences;
** one opened by another process meanwhile is a follower - it captures, it does
** not post - and has the same instance id (the file's): when the sequencer
** process closes, the follower takes over the lease it left, no claim needed */
static void test_sequencer_across_processes(void) {
    TEST("one sequencer per file across processes; a follower takes over when it exits, as the same sequencer");
    reset_dir();
    (void)!system("mkdir -p " L1DIR);
    pid_t pid = spawn_writer("--hold", DIR "/seq.db", DIR, 40);
    if (!wait_file(DIR "/ready", 20000)) { kill(pid, SIGKILL); FAIL("holder did not start"); }
    int hSeq = 0; char hId[65] = {0};
    FILE *f = fopen(DIR "/ready", "r");
    if (!f || fscanf(f, "%d %64s", &hSeq, hId) != 2) { if (f) fclose(f); kill(pid, SIGKILL); FAIL("holder report"); }
    fclose(f);
    HppliteL1 *l1;
    HppliteNode *b = open_shared(DIR "/seq.db", &l1);
    if (!b) { kill(pid, SIGKILL); FAIL("open b"); }
    char bId[65]; hex32(b->instanceId, bId);
    Writer wb = { b->db, "b", 40, 0 };
    write_autocommit(&wb);
    int bFollower = b->follower, bPosted = hpplite_node_flush_batch(b) != 0;
    unsigned char holder[32]; int held = lease_holder(l1, holder);
    char holderHex[65] = {0}; if (held == 1) hex32(holder, holderHex);
    (void)!system("touch " DIR "/stop");
    int status = 0; waitpid(pid, &status, 0);
    unsigned char after[32]; int heldAfter = lease_holder(l1, after);
    char afterHex[65] = {0}; if (heldAfter == 1) hex32(after, afterHex);
    hpplite_node_exec(b, "INSERT INTO t(who, k) VALUES('b', 1000)", NULL);
    uint64_t h = hpplite_node_flush_batch(b);
    int bSeq = !b->follower, bLease = b->hasLease, left = hpplite_node_pending_count(b);
    const char *why = replica_differs(b, l1, DIR "/seq_replica.db");
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(l1);
    if (hSeq != 1) FAIL("the first process should sequence");
    if (!bFollower) FAIL("a second process should be a follower");
    if (bPosted) FAIL("a follower must not post");
    if (strcmp(hId, bId) != 0) FAIL("both processes should be the same sequencer (the file's id)");
    if (held != 1 || strcmp(holderHex, hId) != 0) FAIL("the lease should be the holder's");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) FAIL("holder failed");
    if (heldAfter != 1 || strcmp(afterHex, bId) != 0) FAIL("with a follower still open, the lease is kept, not released");
    if (!h || !bSeq || !bLease) FAIL("the follower should take over and post");
    if (left) FAIL("pending left");
    if (why) FAIL(why);
    PASS();
}

/* Close, alone vs. with company: the lease goes so a rebuilt database (another
** file, another sequencer id) need not wait it out - unless a connection to
** the same file is still open to take over */
static void test_lease_on_close(void) {
    TEST("a sequencer closing alone releases the lease; with another connection open it keeps it");
    reset_dir();
    (void)!system("mkdir -p " L1DIR);
    HppliteL1 *l1;
    HppliteNode *a = open_shared(DIR "/rel.db", &l1);
    if (!a) FAIL("open");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, who, k)", NULL);
    hpplite_node_flush_batch(a);
    hpplite_node_destroy(a);
    unsigned char who[32];
    int heldAlone = lease_holder(l1, who);
    HppliteNode *s = open_node(DIR "/rel.db", l1);
    HppliteNode *f = open_node(DIR "/rel.db", l1);
    int fFollower = f ? f->follower : 0;
    if (s) hpplite_node_exec(s, "INSERT INTO t(who, k) VALUES('s', 1)", NULL);
    if (s) hpplite_node_destroy(s);
    int heldWithCompany = lease_holder(l1, who);
    if (f) hpplite_node_destroy(f);
    hpplite_l1_disconnect(l1);
    if (heldAlone != 0) FAIL("closing alone should release the lease");
    if (!s || !f || !fFollower) FAIL("second connection should be a follower");
    if (heldWithCompany != 1) FAIL("closing with a connection still open should keep the lease");
    PASS();
}

/* keeplease (#265): a sidecar sequencer closing alone lets its lease run instead
** of releasing it, so a wake inside the lease window sends no claim: the instance
** id is the file's and the host's, a restart is the same instance, and the node
** sends nothing while the chain's lease is its own with over half its life left */
static uint64_t lease_expiry(HppliteL1 *l1) {
    HppliteL1State *st = hpplite_l1_get_state(l1);
    uint64_t e = st ? st->leaseExpiry : 0;
    hpplite_l1_state_free(st);
    return e;
}
static void test_keep_lease_on_close(void) {
    TEST("keeplease: a sequencer closing alone keeps its lease; the reopen finds it its own, same expiry, and sends nothing (#265)");
    reset_dir();
    (void)!system("mkdir -p " L1DIR);
    TestNodeOpts o = NODE;
    o.keepLease = 1;
    HppliteL1 *l1 = hpplite_l1_create_mock_dir(L1DIR);
    HppliteNode *a = test_open_node(DIR "/keep.db", l1, &o);
    if (!a) FAIL("open");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, who, k)", NULL);
    hpplite_node_flush_batch(a);
    unsigned char before[32], after[32], again[32];
    int heldBefore = lease_holder(l1, before);
    uint64_t expiryBefore = lease_expiry(l1);
    hpplite_node_destroy(a);
    int heldAfter = lease_holder(l1, after);
    HppliteNode *b = test_open_node(DIR "/keep.db", l1, &o);
    sqlite3_int64 lease = b ? status_int(b->db, "lease") : -1;
    int heldAgain = lease_holder(l1, again);
    uint64_t expiryAgain = lease_expiry(l1);
    if (b) hpplite_node_destroy(b);
    int heldEnd = lease_holder(l1, after);
    hpplite_l1_disconnect(l1);
    if (heldBefore != 1) FAIL("the sequencer should hold the lease after its first flush");
    if (heldAfter != 1 || memcmp(before, after, 32) != 0) FAIL("closing alone with keeplease should leave the lease running, this instance's");
    if (lease != 1 || heldAgain != 1 || memcmp(before, again, 32) != 0 || expiryAgain != expiryBefore) {
        printf("(lease %lld, held %d, expiry %llu -> %llu) ", (long long)lease, heldAgain, (unsigned long long)expiryBefore, (unsigned long long)expiryAgain);
        FAIL("the reopen should find the lease its own and send nothing: same instance, same expiry");
    }
    if (heldEnd != 1) FAIL("the second close keeps it too");
    PASS();
}

/* Another connection changes the schema; this connection's next statement
** finds its schema stale, and SQLite prepares it again and reruns it - the
** first run's log row rolled back with it. The rerun must be logged: it was
** taken for a trigger's sub-program (same statement, same step count) and the
** write never reached the log. */
/* A follower's hpplite_status is what the application sees (php-fpm workers
** are all followers of hpplite-sequencer): it must say what the file says,
** not what the follower read at open (#100, found by the fault harness) */
static void test_follower_status_follows_the_file(void) {
    TEST("a follower's hpplite_status follows the file, not its copy at open");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/fstat.db", l1);
    if (!a) FAIL("open a");
    HppliteNode *b = open_node(DIR "/fstat.db", l1);
    if (!b || !b->follower) FAIL("b should be a follower");
    hpplite_node_exec(a, "CREATE TABLE t(x)", NULL);
    if (hpplite_node_flush_batch(a) != 1) FAIL("batch 1");
    hpplite_node_exec(b, "INSERT INTO t VALUES(1)", NULL);     /* the follower's own write, posted by a */
    if (hpplite_node_flush_batch(a) != 2) FAIL("batch 2");
    sqlite3_int64 hb = status_int(b->db, "height"), ha = status_int(a->db, "height");
    sqlite3_int64 pb = status_int(b->db, "pending");
    uint64_t apiB = hpplite_node_get_height(b);
    hpplite_node_destroy(b);
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l1);
    if (ha != 3) FAILF("the sequencer says height %lld, want 3", (long long)ha);
    if (hb != 3 || apiB != 3) FAILF("the follower says height %lld (API %llu) after the sequencer posted two batches; the file says 3", (long long)hb, (unsigned long long)apiB);
    if (pb != 0) FAILF("the follower says %lld pending, want 0", (long long)pb);
    PASS();
}

static void test_rerun_after_foreign_schema_change(void) {
    TEST("a statement SQLite reruns after another connection's schema change is still captured");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/stale.db", l1);
    HppliteNode *b = open_node(DIR "/stale.db", l1);
    if (!a || !b) FAIL("open");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(b->db, "INSERT INTO t(v) VALUES('prepared before the ALTER')", -1, &ins, NULL) != SQLITE_OK) FAIL("prepare");
    hpplite_node_exec(a, "ALTER TABLE t ADD COLUMN w DEFAULT 'x'", NULL);   /* b's schema is now stale */
    int rc1 = sqlite3_step(ins); sqlite3_finalize(ins);
    hpplite_node_exec(a, "CREATE TABLE u(x)", NULL);
    int rc2 = sqlite3_exec(b->db, "CREATE TABLE w(y)", NULL, NULL, NULL);   /* DDL on a stale schema */
    int rc3 = sqlite3_exec(b->db, "INSERT INTO u VALUES(1)", NULL, NULL, NULL);
    hpplite_node_destroy(b);
    while (hpplite_node_pending_count(a) > 0 && hpplite_node_flush_batch(a) > 0) {}
    const char *why = replica_differs(a, l1, DIR "/stale_replica.db");
    HppliteNode *r = open_node(DIR "/stale_replica2.db", l1);
    sqlite3_int64 hasW = -1;
    if (r) {
        sqlite3_stmt *q;
        if (sqlite3_prepare_v2(r->db, "SELECT count(*) FROM sqlite_schema WHERE name IN ('w', 'u')", -1, &q, NULL) == SQLITE_OK && sqlite3_step(q) == SQLITE_ROW) hasW = sqlite3_column_int64(q, 0);
        sqlite3_finalize(q);
        hpplite_node_destroy(r);
    }
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l1);
    if (rc1 != SQLITE_DONE || rc2 != SQLITE_OK || rc3 != SQLITE_OK) FAIL("writes failed");
    if (hasW != 2) FAIL("the replica lacks a table created on a stale schema");
    if (why) FAIL(why);
    PASS();
}

/* The same inside a transaction the application opened: only the failed
** run's own statement rolls back there, but its end (no rows changed)
** unlogged the statement, and the rerun went to the database alone */
static void test_rerun_in_transaction(void) {
    TEST("the same inside the application's transaction: the rerun is still captured");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *a = open_node(DIR "/stale_tx.db", l1);
    HppliteNode *b = open_node(DIR "/stale_tx.db", l1);
    if (!a || !b) FAIL("open");
    hpplite_node_exec(a, "CREATE TABLE t(id INTEGER PRIMARY KEY, v)", NULL);
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(b->db, "INSERT INTO t(v) VALUES('prepared before the ALTER')", -1, &ins, NULL) != SQLITE_OK) FAIL("prepare");
    hpplite_node_exec(a, "ALTER TABLE t ADD COLUMN w DEFAULT 'x'", NULL);   /* b's schema is now stale */
    int rc0 = sqlite3_exec(b->db, "BEGIN", NULL, NULL, NULL);
    int rc1 = sqlite3_step(ins); sqlite3_finalize(ins);
    int rc2 = sqlite3_exec(b->db, "INSERT INTO t(v) VALUES('after it'); COMMIT", NULL, NULL, NULL);
    hpplite_node_destroy(b);
    while (hpplite_node_pending_count(a) > 0 && hpplite_node_flush_batch(a) > 0) {}
    const char *why = replica_differs(a, l1, DIR "/stale_tx_replica.db");
    hpplite_node_destroy(a);
    hpplite_l1_disconnect(l1);
    if (rc0 != SQLITE_OK || rc1 != SQLITE_DONE || rc2 != SQLITE_OK) FAIL("writes failed");
    if (why) FAIL(why);
    PASS();
}

int main(int argc, char **argv) {
    self_path = argv[0];
    test_suite_init("conc", PRIVKEY, DIR);
    if (argc == 5 && strcmp(argv[1], "--hold") == 0) return holder_main(argv);
    if (argc == 5 && strncmp(argv[1], "--", 2) == 0) return writer_main(argv);
    printf("HPPLite concurrent writers\n");
    test_two_connections_two_threads();
    test_two_processes();
    test_mixed_threads_and_processes();
    test_sequencer_across_processes();
    test_lease_on_close();
    test_keep_lease_on_close();
    test_rerun_after_foreign_schema_change();
    test_rerun_in_transaction();
    test_follower_status_follows_the_file();
    return test_report();
}
