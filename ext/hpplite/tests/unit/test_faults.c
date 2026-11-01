/*
** Fault injection (#98): a random workload under random failures of the
** parts that fail in production - the chain (a post refused, a confirmation
** lost, a transaction pending past the timeout, a claim that reverts, a
** reorg, the lease expiring) and the file (a bookkeeping commit that fails
** as SQLITE_FULL) - with one oracle at the end: the chain holds exactly the
** writer's batches, nothing twice and nothing missing, and a replica
** rebuilt from the chain equals the writer, with every posted root matching.
**
** Every failure the reviews found by reading (#90, #97, #40, #34) has the
** shape "a step failed after another succeeded, and memory and the file (or
** the chain) disagreed". This runs those shapes thousands of times.
**
**   HPPLITE_FAULT_SEEDS=200 ./test_faults     (default 60)
**   HPPLITE_FAULT_SEED=17 ./test_faults       (one seed, verbose)
*/

#include "test_lib.h"
#include "l1_interface.h"
#include <strings.h>
#include <unistd.h>

/* in a scratch directory of this process's own (test_suite_init): two
** harness runs at once (a long seed sweep beside ctest) must not share one */
static char g_wdb[600], g_rdb[600];

static const unsigned char PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20 };

static int g_verbose;
static unsigned g_rng;
static unsigned rnd(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5; return g_rng; }
static int chance(int percent) { return (int)(rnd() % 100) < percent; }

static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1, int interval) {
    TestNodeOpts o = { .checkpointInterval = interval };
    return test_open_node(dbPath, l1, &o);
}

/* Every row of every application table, in order, typed */
#define dump(db) test_dump(db, TEST_DUMP_SKIP_SQLITE)

enum { F_NONE, F_SUBMIT_FAILS, F_CONFIRMATION_LOST, F_TX_PENDING, F_CLAIM_REVERTS, F_COMMIT_FAILS, F_REORG, F_LEASE_EXPIRES, F_RPC_DOWN, F_COUNT };
static const char *fault_name[] = { "none", "submit fails", "confirmation lost", "tx pending", "claim reverts", "commit fails", "reorg", "lease expires", "rpc down" };

static void arm(HppliteL1 *l1, HppliteNode *n, int f) {
    uint64_t last = 0, total = 0;
    switch (f) {
        case F_SUBMIT_FAILS:      hpplite_l1_mock_set_fail_submit(l1, 1); break;
        case F_CONFIRMATION_LOST: hpplite_l1_mock_set_lose_confirmation(l1, 1); break;
        case F_TX_PENDING:        hpplite_l1_mock_set_tx_pending(l1, 1); break;
        case F_CLAIM_REVERTS:
            /* only bites when a claim is needed: let the lease run out first */
            hpplite_l1_mock_advance_time(l1, 400);
            n->leaseRecheckAt = 0;
            hpplite_l1_mock_set_claim_reverts(l1, 1);
            break;
        case F_COMMIT_FAILS:      hpplite_test_fail_commits = 1; break;
        case F_REORG:
            /* one to four batches gone, within what the node keeps (HPPLITE_POSTED_KEEP) */
            hpplite_l1_get_da_state(l1, &last, &total, NULL);
            if (last >= 2) { uint64_t depth = 1 + rnd() % 4; hpplite_l1_mock_truncate(l1, depth < last ? last - depth : 1); }
            break;
        case F_LEASE_EXPIRES:     hpplite_l1_mock_advance_time(l1, 400); n->leaseRecheckAt = 0; break;
        case F_RPC_DOWN:          hpplite_l1_mock_set_rpc_down(l1, 1); break;
        default: break;
    }
}
static void disarm(HppliteL1 *l1) {
    hpplite_l1_mock_set_fail_submit(l1, 0);
    hpplite_l1_mock_set_lose_confirmation(l1, 0);
    hpplite_l1_mock_set_tx_pending(l1, 0);
    hpplite_l1_mock_set_claim_reverts(l1, 0);
    hpplite_l1_mock_set_rpc_down(l1, 0);
    hpplite_test_fail_commits = 0;
}

static int exec(HppliteNode *n, const char *sql) {
    char *err = NULL;
    int rc = hpplite_node_exec(n, sql, &err);
    if (rc != SQLITE_OK && g_verbose) printf("    [%s: %s]\n", sql, err ? err : "?");
    sqlite3_free(err);
    return rc;
}

/* One seed: the workload, the faults, the oracle. Returns NULL or why.
** With two connections, a second node on the same file (a follower: it
** captures, the first posts) takes half the writes, and outlives the
** sequencer at the end: it promotes itself and posts what is left. */
static const char *run_seed(unsigned seed, int two) {
    static char why[600];
    int nFaults[F_COUNT] = {0};
    int altered = 0, step, nextId = 1, nRows = 0;
    g_rng = seed * 2654435761u + 12345;
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(g_wdb, l1, 1 + (int)(rnd() % 4));
    if (!w) return "open writer";
    exec(w, "CREATE TABLE t(id INTEGER PRIMARY KEY, v TEXT, k INTEGER)");
    exec(w, "CREATE INDEX tk ON t(k)");
    exec(w, "CREATE TABLE s(name TEXT PRIMARY KEY, n INTEGER) WITHOUT ROWID");
    HppliteNode *f = NULL;
    if (two) {
        f = open_node(g_wdb, l1, 1);
        if (!f || !f->follower) { if (f) hpplite_node_destroy(f); hpplite_node_destroy(w); hpplite_l1_disconnect(l1); return "second connection should be a follower"; }
    }

    for (step = 0; step < 50; step++) {
        int op = (int)(rnd() % 10);
        HppliteNode *c = (f && chance(50)) ? f : w;                   /* who writes this step */
        char sql[256];
        if (op < 4) {
            snprintf(sql, sizeof sql, "INSERT INTO t(v, k) VALUES('row %d of seed %u', %d)", nextId, seed, (int)(rnd() % 7));
            if (exec(c, sql) == SQLITE_OK) { nextId++; nRows++; }
        } else if (op < 6) {
            snprintf(sql, sizeof sql, "UPDATE t SET v = v || '!', k = k + 1 WHERE id = %d", 1 + (int)(rnd() % (nextId > 1 ? nextId - 1 : 1)));
            exec(c, sql);
        } else if (op == 6) {
            snprintf(sql, sizeof sql, "DELETE FROM t WHERE id = %d", 1 + (int)(rnd() % (nextId > 1 ? nextId - 1 : 1)));
            exec(c, sql);
        } else if (op == 7) {
            exec(c, "BEGIN");
            snprintf(sql, sizeof sql, "INSERT INTO s(name, n) VALUES('k%d', %d) ON CONFLICT(name) DO UPDATE SET n = n + 1", (int)(rnd() % 5), step);
            exec(c, sql);
            snprintf(sql, sizeof sql, "INSERT INTO t(v, k) VALUES('in txn %d', %d)", step, (int)(rnd() % 7));
            if (exec(c, sql) == SQLITE_OK) nextId++;
            exec(c, "UPDATE t SET k = k WHERE 0");                 /* a no-op in a transaction */
            if (chance(20)) exec(c, "ROLLBACK"); else exec(c, "COMMIT");
            if (nextId > 1) nextId = (int)scalar(c->db, "SELECT coalesce(max(id), 0) + 1 FROM t");
        } else if (op == 8 && !altered) {
            exec(c, "ALTER TABLE t ADD COLUMN extra TEXT DEFAULT 'e'");   /* text-logged, rebuilds the triggers */
            altered = 1;
        } else {
            exec(c, "DELETE FROM s WHERE n < 0");                        /* a no-op in autocommit */
        }

        if (chance(45)) {
            int f = chance(60) ? (int)(rnd() % F_COUNT) : F_NONE;
            nFaults[f]++;
            if (g_verbose) printf("    step %d: flush under '%s'\n", step, fault_name[f]);
            arm(l1, w, f);
            hpplite_node_flush_batch(w);
            disarm(l1);
            if (chance(50)) hpplite_node_flush_batch(w);       /* the retry, as the timer would */
        }
    }

    /* the end of the session: everything must reach the chain. With two
    ** connections the sequencer leaves first, under one last fault, and the
    ** follower takes over */
    disarm(l1);
    if (f) {
        int last = chance(50) ? (int)(1 + rnd() % (F_COUNT - 1)) : F_NONE;
        nFaults[last]++;
        if (g_verbose) printf("    sequencer closes under '%s'; the follower takes over\n", fault_name[last]);
        arm(l1, w, last);
        hpplite_node_destroy(w);
        disarm(l1);
        w = f; f = NULL;
    }
    hpplite_node_flush_batch(w);                   /* the timer's next tick: repairs a reorg with nothing pending too */
    for (int i = 0; i < 40 && hpplite_node_pending_count(w) > 0; i++) {
        if (i == 10 || i == 25) { hpplite_l1_mock_advance_time(l1, 400); w->leaseRecheckAt = 0; }
        hpplite_node_flush_batch(w);
    }
    if (hpplite_node_pending_count(w) != 0) {
        snprintf(why, sizeof why, "seed %u: %d rows still pending after the faults were lifted (state %s)", seed,
                 hpplite_node_pending_count(w), w->state == HPPLITE_STATE_ERROR ? "error" : "ok");
        hpplite_node_destroy(w); hpplite_l1_disconnect(l1); return why;
    }
    uint64_t last = 0, total = 0;
    hpplite_l1_get_da_state(l1, &last, &total, NULL);
    uint64_t heightW = (uint64_t)scalar(w->db, "SELECT next_height FROM _hpplite_meta");   /* the file's */
    uint64_t statusW = (uint64_t)scalar(w->db, "SELECT height FROM hpplite_status");       /* what the connection says */
    if (last + 1 != heightW) {
        snprintf(why, sizeof why, "seed %u: the chain holds %llu batches, the file says %llu finalized", seed,
                 (unsigned long long)last, (unsigned long long)heightW - 1);
        hpplite_node_destroy(w); hpplite_l1_disconnect(l1); return why;
    }
    if (statusW != heightW) {
        snprintf(why, sizeof why, "seed %u: hpplite_status.height says %llu, the file %llu (a %s)", seed,
                 (unsigned long long)statusW, (unsigned long long)heightW, two ? "connection that was a follower" : "sequencer");
        hpplite_node_destroy(w); hpplite_l1_disconnect(l1); return why;
    }
    char *a = dump(w->db);
    unsigned char rootW[32], rootR[32];
    hpplite_state_root(w->db, rootW);
    hpplite_node_destroy(w);

    HppliteNode *r = open_node(g_rdb, l1, 1);
    if (!r) { sqlite3_free(a); hpplite_l1_disconnect(l1); return "open replica"; }
    char *b = dump(r->db);
    hpplite_state_root(r->db, rootR);
    int errState = r->state == HPPLITE_STATE_ERROR;
    unsigned long long mism = (unsigned long long)r->ctx->nRootMismatches, cmp = (unsigned long long)r->ctx->nRootsCompared;
    unsigned long long skipped = (unsigned long long)r->ctx->nReplaySkipped, dropped = (unsigned long long)r->ctx->nTxnsDropped;
    uint64_t heightR = hpplite_node_get_height(r);
    hpplite_node_destroy(r);
    hpplite_l1_disconnect(l1);
    int same = strcmp(a, b) == 0;
    if (!same) {
        size_t i = test_dump_diff(a, b);
        snprintf(why, sizeof why, "seed %u: replica differs: writer [%.60s] replica [%.60s]", seed, a + i, b + i);
    } else if (errState || heightR != heightW) {
        snprintf(why, sizeof why, "seed %u: replica %s at height %llu, writer at %llu", seed, errState ? "in error" : "stopped",
                 (unsigned long long)heightR, (unsigned long long)heightW);
    } else if (memcmp(rootW, rootR, 32) != 0) {
        snprintf(why, sizeof why, "seed %u: rows equal, state root differs", seed);
    } else if (mism) {
        snprintf(why, sizeof why, "seed %u: %llu of %llu posted roots mismatch on replay", seed, mism, cmp);
    } else if (skipped || dropped) {
        snprintf(why, sizeof why, "seed %u: %llu statements skipped, %llu transactions dropped on replay", seed, skipped, dropped);
    }
    sqlite3_free(a); sqlite3_free(b);
    if (g_verbose) {
        printf("    faults:");
        for (int f = 1; f < F_COUNT; f++) if (nFaults[f]) printf(" %s x%d", fault_name[f], nFaults[f]);
        printf("; %d rows, %llu batches, %llu roots compared\n", nRows, (unsigned long long)last, cmp);
    }
    return same && !errState && heightR == heightW && !memcmp(rootW, rootR, 32) && !mism && !skipped && !dropped ? NULL : why;
}

int main(void) {
    const char *one = getenv("HPPLITE_FAULT_SEED"), *many = getenv("HPPLITE_FAULT_SEEDS");
    test_suite_init("faults", PRIVKEY, NULL);
    snprintf(g_wdb, sizeof g_wdb, "%s", tpath("w.db"));
    snprintf(g_rdb, sizeof g_rdb, "%s", tpath("r.db"));
    unsigned nSeeds = many ? (unsigned)atoi(many) : 60, failed = 0;
    printf("=== HPPLite under injected faults (#98), %s capture ===\n",
           getenv("HPPLITE_CAPTURE") && strcasecmp(getenv("HPPLITE_CAPTURE"), "text") == 0 ? "statement" : "row");
    if (one) {
        g_verbose = 1;
        const char *why = run_seed((unsigned)atoi(one), 0);
        printf("one connection: %s\n", why ? why : "ok");
        const char *why2 = run_seed((unsigned)atoi(one), 1);
        printf("two connections: %s\n", why2 ? why2 : "ok");
        remove_dir();
        return why || why2 ? 1 : 0;
    }
    for (unsigned s = 1; s <= nSeeds; s++) {
        const char *why = run_seed(s, 0);
        if (why) { printf("  FAIL (one connection): %s\n", why); failed++; }
        why = run_seed(s, 1);
        if (why) { printf("  FAIL (two connections): %s\n", why); failed++; }
    }
    remove_dir();
    printf("\n%u/%u seeds passed\n", nSeeds - failed, nSeeds);
    return failed ? 1 : 0;
}
