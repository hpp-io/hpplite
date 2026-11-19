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

#define DIR "/tmp/hpplite_test_memory"
#define DB_MB 64
#define BOUND_MB 16
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

static const char *self;

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
        execl(self, self, "child", which, (char *)NULL);
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

int main(int argc, char **argv) {
    self = argv[0];
    test_suite_init("memory", NULL, DIR);
    if (argc > 2 && strcmp(argv[1], "child") == 0) {
        if (strcmp(argv[2], "open") == 0) return child_open();
        if (strcmp(argv[2], "backlog") == 0) return child_backlog();
        return 2;
    }
    printf("=== HPPLite memory against the database's size ===\n");
    reset_dir();
    test_open_is_not_the_database();
    test_backlog_is_not_in_memory();
    remove_dir();
    return test_report();
}
