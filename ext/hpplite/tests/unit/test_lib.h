/*
** HPPLite test library (#106)
**
** What every suite under tests/ needs and used to carry its own copy of:
** the TEST/PASS/FAIL macros, a scratch directory, the node open helper,
** the row dump that decides "replica equal", the replica check, the
** hpplite_status helpers. test_util.h keeps the keys and the factory
** address for the real-chain tests and examples.
**
** A suite starts with test_suite_init(): its node id, its key and its
** scratch directory. Everything else takes no suite state but the options
** a case passes.
*/
#ifndef HPPLITE_TEST_LIB_H
#define HPPLITE_TEST_LIB_H

#include "node.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The tally: TEST at the top of a case, PASS at its end, FAIL/FAILF on the
** way out (they return from a void case) */
extern int tests_run, tests_passed;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; fflush(stdout); } while(0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while(0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while(0)
#define FAILF(...) do { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); return; } while(0)

/* Row capture (#19) is the default; cases about statement capture run with
** HPPLITE_CAPTURE=text (the _text tests) and skip otherwise */
int test_rows_mode(void);
int test_pages_mode(void);                   /* HPPLITE_CAPTURE=pages: page capture (#146) */
#define SKIP_IF_ROWS(why) do { if (test_rows_mode()) { printf("SKIP (text capture: %s) ", why); PASS(); return; } } while(0)
/* Cases about statements, the log table or the content root do not apply to page capture (#146) */
#define SKIP_IF_PAGES(why) do { if (test_pages_mode()) { printf("SKIP (page capture: %s) ", why); PASS(); return; } } while(0)
/* pending is n commits; in page mode the file as opened (its pages, never shipped) may count as one more */
int test_pending_is(long long pending, int n);

/* A TCP port on 127.0.0.1 free right now, from the OS (bind to port 0), for
** a service a suite starts: never one derived from the pid, which another
** program may hold (#177). -1 when there is none */
int test_free_port(void);
/* A service the suite forked is still running: a probe that gets an answer
** after it exited is talking to someone else's server (#177) */
int test_child_alive(pid_t pid);

/* The end of main: the tally, and the process's exit code */
int test_report(void);

/* The bytes an encrypt=off node posts, as text: plain JSON, or since #12 the
** HPZ1 frame around deflated JSON, inflated. NUL-terminated; free() */
uint8_t *test_readable_bytes(const uint8_t *raw, size_t len, size_t *pLen);

/* The suite: the node id and key its nodes get, and its scratch directory.
** dir NULL: /tmp/hpplite_test_<nodeId>.<pid>, this process's own, so two
** runs at once (a seed sweep beside ctest) do not share one. privkey NULL:
** the library's key. */
void test_suite_init(const char *nodeId, const unsigned char privkey[32], const char *dir);
const char *test_dir(void);
const char *tpath(const char *file);         /* "<dir>/<file>", in one of four static buffers */
void reset_dir(void);                        /* rm -rf, mkdir -p */
void remove_dir(void);

/* SQL on a connection */
extern int test_verbose;                     /* exec_sql prints what failed */
sqlite3_int64 scalar(sqlite3 *db, const char *sql);           /* the first column of the first row; -1 when there is none */
void text_of(sqlite3 *db, const char *sql, char *out, size_t n); /* ...as text; "" when there is none */
int count_rows(sqlite3 *db, const char *table);               /* -1 when the table is missing */
int exec_sql(sqlite3 *db, const char *sql);                   /* sqlite3_exec's rc */
int run(HppliteNode *n, const char **aSql);                   /* each on the application connection; -1 and a note on the first failure */
char *query_text(sqlite3 *db, const char *sql);               /* every row, columns joined by '|', rows by '\n'; sqlite3_free */

/* A node: created on dbPath with the suite's id and key, then, with an L1,
** attached to it, synced from it and started (hpplite_node_start). Options
** at zero are the node's own, but for the chain: local-only when there is
** no L1 (#38), not started without one. Most suites set TEST_INTERVAL_1000. */
#define TEST_INTERVAL_1000 1000              /* checkpointInterval the suites usually want: a checkpoint (a root) every 1000 batches, not the node's every 100 */
typedef struct TestNodeOpts {
    int checkpointInterval;                  /* as HppliteNodeConfig: 0 = the node's own default (100); TEST_INTERVAL_1000 is the suites' usual */
    size_t maxBatchBytes;                    /* the node's DA limit; the mock takes it as the contract's */
    size_t daLimit;                          /* the mock's DA limit alone, the node left to find it */
    int noEncrypt;                           /* plaintext batches */
    int noCompress;                          /* the batch JSON as is (#12) */
    int localOnly;                           /* 0: when there is no L1; 1: always; -1: never (no chain: the log stays pending) */
    int verifyOnly;                          /* role=verify (#5) */
    int determinismWarn;                     /* warn about non-deterministic SQL instead of refusing it */
    int captureText;                         /* statement capture, as v0.1 */
    int capturePages;                        /* page capture (#146); also from HPPLITE_CAPTURE=pages */
    int noSync;                              /* attach the L1 without syncing from it first */
    int start;                               /* start the node even without an L1 */
    const char *nodeId;                      /* NULL: the suite's */
    const unsigned char *privkey;            /* NULL: the suite's */
    const char *storeUrl;                    /* store= (#11): the batch bytes go there, the chain holds commitments */
    uint64_t storeCap;                       /* storecap= */
    int storeFallbackOff;                    /* storefallback=off: a store that does not answer keeps the batch waiting */
    int snapshotMs;                          /* snapshot= (#221): a snapshot at an anchor when the last is older */
    int fromSnapshot;                        /* from=snapshot (#222): a new replica starts from the latest snapshot */
} TestNodeOpts;
HppliteNode *test_open_node(const char *dbPath, HppliteL1 *l1, const TestNodeOpts *o);
int same_root(HppliteNode *a, HppliteNode *b);

/* Every row of every application table, typed and in full (integers as
** i<n>, floats as f<bits>, text and blobs as t<hex>/b<hex>, NULL as n), a
** rowid table with its rowids in rowid order, a WITHOUT ROWID table (and
** SQLite's own, sqlite_sequence) in the order of all its columns; _hpplite_*
** and virtual tables left out. The
** writer's and a replica's must be equal, byte for byte. sqlite3_free. */
#define TEST_DUMP_NO_ROWID        1          /* rowid tables without their rowids, in column order */
#define TEST_DUMP_SKIP_FTS_SHADOW 2          /* no %_data, %_idx, %_docsize, %_config, %_content */
#define TEST_DUMP_SKIP_SQLITE     4          /* no sqlite_* (sqlite_sequence) */
char *test_dump(sqlite3 *db, unsigned flags);
/* Where two dumps part: the offset of the row (or table) the first difference is in */
size_t test_dump_diff(const char *a, const char *b);

/* The check a writer's case ends with: a replica rebuilt from L1 alone into
** replicaPath (opened with replicaOpts) must equal the writer - every row
** (test_dump with dumpFlags) and the state root of the content. flushRounds:
** flush the writer first, that many times at most, until nothing is pending
** (rows left are a failure); 0: the caller flushed. destroyWriter: the
** writer is closed before the replica opens (a sequencer that shut down),
** else it stays open, the caller's. NULL, or why. */
const char *test_replica_check(HppliteNode *w, HppliteL1 *l1, const char *replicaPath, const TestNodeOpts *replicaOpts,
                               unsigned dumpFlags, int flushRounds, int destroyWriter);

/* hpplite_status, one column at a time */
const char *status_text(sqlite3 *db, const char *col);        /* "" for NULL or no table */
sqlite3_int64 status_int(sqlite3 *db, const char *col);       /* -1 for no table */
#define WANT_STATUS(db, col, want, when) do { \
    const char *got_ = status_text(db, col); \
    if (strcmp(got_, want) != 0) FAILF("%s: %s is '%s', want '%s'", when, col, got_, want); } while (0)
#define WANT_STATUS_PREFIX(db, col, want, when) do { \
    const char *got_ = status_text(db, col); \
    if (strncmp(got_, want, strlen(want)) != 0) FAILF("%s: %s is '%s', want '%s...'", when, col, got_, want); } while (0)

#endif /* HPPLITE_TEST_LIB_H */
