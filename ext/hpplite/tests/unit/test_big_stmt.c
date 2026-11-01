/*
** HPPLite statements larger than one batch (#59)
**
** A multi-row INSERT an ORM generates can be larger than a whole batch (HPP
** Sepolia: ~40 KB per transaction). Its text is cut into pieces across
** batches, the way a large transaction is (#57); a replica joins the pieces
** and runs the statement once, whole. The invariants: no posted batch over
** the limit; a replica never runs part of a statement; a crash or a lost
** confirmation between pieces loses nothing; the replica reaches the
** writer's root.
*/

#include "test_lib.h"
#include "batch.h"

static const unsigned char PRIVKEY[32] = {
    0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xcb,0xcc,0xcd,0xce,0xcf,0xd0,
    0xd1,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xdb,0xdc,0xdd,0xde,0xdf,0xe0
};
#define DIR "/tmp/hpplite_test_big_stmt"
#define LIMIT 3000

static const TestNodeOpts NODE = { .maxBatchBytes = LIMIT, .checkpointInterval = TEST_INTERVAL_1000 };
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return test_open_node(dbPath, l1, &NODE); }
/* INSERT INTO p VALUES (...),(...) ... with n rows: quotes, escapes and
** multi-byte UTF-8 in it, so a cut has to respect all three */
static char *big_insert(int base, int n) {
    sqlite3_str *s = sqlite3_str_new(NULL);
    sqlite3_str_appendall(s, "INSERT INTO p(id, name, note) VALUES");
    for (int i = 0; i < n; i++) {
        sqlite3_str_appendf(s, "%s(%d, 'product \"%d\" \\ caf\xc3\xa9 \xe2\x82\xac', 'line one\nline two %d')",
                            i ? "," : "", base + i, base + i, base + i);
    }
    return sqlite3_str_finish(s);
}
static int max_batch_len(HppliteL1 *l1, uint64_t last) {
    size_t mx = 0;
    for (uint64_t h = 1; h <= last; h++) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, h, &len);
        free(raw);
        if (len > mx) mx = len;
    }
    return (int)mx;
}

static void test_format(void) {
    TEST("\"frag\" in the JSON and the block hash only when set, round-trip");
    HppliteBatch *b = hpplite_batch_new(4);
    hpplite_batch_add_stmt(b, "INSERT INTO p VALUES(1, 'piece", -1, 0);
    unsigned char prev[32] = {0}, h0[32], h1[32];
    char *plain = hpplite_batch_to_json(b);
    hpplite_block_hash(b, prev, h0);
    b->aTxns[0].frag = 1;
    hpplite_block_hash(b, prev, h1);
    char *json = hpplite_batch_to_json(b);
    HppliteBatch *p = hpplite_batch_from_json(json);
    int ok = p && p->nTxns == 1 && p->aTxns[0].frag == 1 && strcmp(p->aTxns[0].zSql, "INSERT INTO p VALUES(1, 'piece") == 0;
    int none = strstr(plain, "\"frag\"") == NULL;
    sqlite3_free(plain); sqlite3_free(json);
    hpplite_batch_free(b); hpplite_batch_free(p);
    if (!none) FAIL("frag written when unset");
    if (!ok) FAIL("frag did not round-trip");
    if (memcmp(h0, h1, 32) == 0) FAIL("frag not in the block hash");
    PASS();
}

static void test_statement_over_batches(void) {
    TEST("a 60 KB statement is accepted, posted across batches within the limit, and replays once");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE p(id INTEGER PRIMARY KEY, name TEXT, note TEXT)", NULL);
    hpplite_node_exec(w, "CREATE TABLE s(x)", NULL);
    char *sql = big_insert(1, 600);
    size_t len = strlen(sql);
    hpplite_node_exec(w, "INSERT INTO s VALUES('before')", NULL);
    int rc = sqlite3_exec(w->db, sql, NULL, NULL, NULL);
    sqlite3_free(sql);
    hpplite_node_exec(w, "INSERT INTO s VALUES('after')", NULL);
    if (rc != SQLITE_OK) { hpplite_node_destroy(w); FAILF("the %zu-byte statement was refused", len); }
    uint64_t last = hpplite_node_flush_batch(w);
    int left = hpplite_node_pending_count(w);
    int mx = max_batch_len(l1, last);
    HppliteNode *r = open_node(DIR "/r.db", l1);
    int rows = r ? count_rows(r->db, "p") : -1, srows = r ? count_rows(r->db, "s") : -1;
    int same = r && same_root(w, r);
    int skipped = r ? (int)r->ctx->nReplaySkipped : -1;
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (left != 0) FAILF("%d statements left pending", left);
    if (last < 10) FAILF("only %llu batches for a %zu-byte statement", (unsigned long long)last, len);
    if (mx > LIMIT) FAILF("a batch is %d bytes, over %d", mx, LIMIT);
    if (rows != 600 || srows != 2) FAILF("replica rows %d/%d", rows, srows);
    if (skipped) FAIL("the replica skipped a statement (ran a piece?)");
    if (!same) FAIL("replica root differs");
    PASS();
}

static void test_statement_in_transaction_and_reader_between(void) {
    TEST("pieces inside a transaction: a replica reopened after every batch never shows part of it");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE p(id INTEGER PRIMARY KEY, name TEXT, note TEXT)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    char *a = big_insert(1, 200), *b = big_insert(1001, 200);
    hpplite_node_exec(w, "BEGIN", NULL);
    int rc = sqlite3_exec(w->db, a, NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_exec(w->db, "INSERT INTO p VALUES(500, 'small', 'between')", NULL, NULL, NULL);
    if (rc == SQLITE_OK) rc = sqlite3_exec(w->db, b, NULL, NULL, NULL);
    hpplite_node_exec(w, "COMMIT", NULL);
    sqlite3_free(a); sqlite3_free(b);
    if (rc != SQLITE_OK) { hpplite_node_destroy(w); FAIL("refused"); }
    uint64_t last = hpplite_node_flush_batch(w);
    HppliteL1 *l2 = hpplite_l1_create_mock();
    hpplite_l1_mock_set_batch_size_limit(l2, LIMIT);
    for (uint64_t h = 1; h <= last; h++) {
        size_t len = 0;
        uint8_t *raw = hpplite_l1_get_batch(l1, h, &len);
        if (!raw) FAIL("copy");
        hpplite_l1_mock_inject_batch(l2, raw, len);
        free(raw);
        HppliteNode *r = open_node(DIR "/r.db", l2);
        int rows = r ? count_rows(r->db, "p") : -2;
        if (r) hpplite_node_destroy(r);
        if (rows != 0 && rows != 401 && rows != -1) FAILF("after batch %llu the replica shows %d rows", (unsigned long long)h, rows);
        if (h == last && rows != 401) FAILF("end: %d rows", rows);
    }
    HppliteNode *r = open_node(DIR "/r.db", l2);
    int same = r && same_root(w, r);
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    hpplite_l1_disconnect(l2);
    if (!same) FAIL("replica root differs");
    PASS();
}

static void test_crash_between_pieces(void) {
    TEST("the writer dies between pieces: reopen continues from the right byte");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE p(id INTEGER PRIMARY KEY, name TEXT, note TEXT)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    char *sql = big_insert(1, 300);
    if (sqlite3_exec(w->db, sql, NULL, NULL, NULL) != SQLITE_OK) { sqlite3_free(sql); hpplite_node_destroy(w); FAIL("refused"); }
    sqlite3_free(sql);
    hpplite_l1_mock_set_fail_submit_after(l1, 3);          /* three pieces land, then the writer is gone */
    hpplite_node_flush_batch(w);
    hpplite_node_destroy(w);
    hpplite_l1_mock_set_fail_submit(l1, 0);
    w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("reopen");
    hpplite_node_flush_batch(w);
    int left = hpplite_node_pending_count(w);
    HppliteNode *r = open_node(DIR "/r.db", l1);
    int rows = r ? count_rows(r->db, "p") : -1;
    int same = r && same_root(w, r);
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (left) FAIL("pending left");
    if (rows != 300 || !same) FAILF("replica rows %d", rows);
    PASS();
}

static void test_lost_confirmation_of_a_piece(void) {
    TEST("a piece landed but its confirmation was lost: adopted, the next piece follows it");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE p(id INTEGER PRIMARY KEY, name TEXT, note TEXT)", NULL);
    if (hpplite_node_flush_batch(w) != 1) FAIL("batch 1");
    char *sql = big_insert(1, 300);
    if (sqlite3_exec(w->db, sql, NULL, NULL, NULL) != SQLITE_OK) { sqlite3_free(sql); hpplite_node_destroy(w); FAIL("refused"); }
    sqlite3_free(sql);
    hpplite_l1_mock_set_lose_confirmation(l1, 1);
    hpplite_node_flush_batch(w);
    hpplite_l1_mock_set_lose_confirmation(l1, 0);
    hpplite_node_destroy(w);
    w = open_node(DIR "/w.db", l1);
    if (!w || w->state == HPPLITE_STATE_ERROR) FAIL("reopen");
    hpplite_node_flush_batch(w);
    int left = hpplite_node_pending_count(w);
    HppliteNode *r = open_node(DIR "/r.db", l1);
    int rows = r ? count_rows(r->db, "p") : -1;
    int same = r && same_root(w, r);
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (left) FAIL("pending left");
    if (rows != 300 || !same) FAILF("replica rows %d", rows);
    PASS();
}

int main(void) {
    printf("=== HPPLite statements across batches (#59) ===\n");
    test_suite_init("bigstmt", PRIVKEY, DIR);
    test_format();
    test_statement_over_batches();
    test_statement_in_transaction_and_reader_between();
    test_crash_between_pieces();
    test_lost_confirmation_of_a_piece();
    return test_report();
}
