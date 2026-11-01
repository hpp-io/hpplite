/*
** A chain reorganisation drops batches HPPLite already finalized (#49)
**
** The writer's height is then ahead of L1, and every later submission is
** "non-sequential". The writer keeps the bytes of its recent batches and
** posts the lost heights again, byte for byte, then carries on; a batch it no
** longer keeps cannot be restored, and it stops rather than post past a gap.
*/
#include "test_lib.h"

static const unsigned char PRIVKEY[32] = {
    0xa1,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xab,0xac,0xad,0xae,0xaf,0xb0,
    0xb1,0xb2,0xb3,0xb4,0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xbb,0xbc,0xbd,0xbe,0xbf,0xc0
};
#define DIR "/tmp/hpplite_test_reorg"
static const TestNodeOpts NODE = { .checkpointInterval = TEST_INTERVAL_1000 };
static HppliteNode *open_node(const char *dbPath, HppliteL1 *l1) { return test_open_node(dbPath, l1, &NODE); }
static void write_and_flush(HppliteNode *n, int i) {
    char sql[64];
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES(%d)", i);
    hpplite_node_exec(n, sql, NULL);
    hpplite_node_flush_batch(n);
}

static void test_lost_batches_reposted(void) {
    TEST("a reorg drops the last two batches: they are posted again, byte for byte, and writing continues");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    for (int i = 1; i <= 4; i++) write_and_flush(w, i);     /* heights 1..4 */
    if (hpplite_node_get_height(w) != 5) FAIL("test premise: 4 batches");
    size_t len3 = 0, len4 = 0;
    uint8_t *b3 = hpplite_l1_get_batch(l1, 3, &len3), *b4 = hpplite_l1_get_batch(l1, 4, &len4);
    hpplite_l1_mock_truncate(l1, 2);                         /* the reorg */
    write_and_flush(w, 5);
    hpplite_node_flush_batch(w);
    uint64_t onL1 = 0, total = 0; unsigned char lh[32];
    hpplite_l1_get_da_state(l1, &onL1, &total, lh);
    size_t n3 = 0, n4 = 0;
    uint8_t *c3 = hpplite_l1_get_batch(l1, 3, &n3), *c4 = hpplite_l1_get_batch(l1, 4, &n4);
    int same3 = c3 && b3 && n3 == len3 && memcmp(c3, b3, n3) == 0;
    int same4 = c4 && b4 && n4 == len4 && memcmp(c4, b4, n4) == 0;
    free(b3); free(b4); free(c3); free(c4);
    int pending = hpplite_node_pending_count(w), state = w->state;
    HppliteNode *r = open_node(DIR "/r.db", l1);
    int rows = r ? count_rows(r->db, "t") : -1;
    if (r) hpplite_node_destroy(r);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (state == HPPLITE_STATE_ERROR) FAIL("the writer gave up");
    if (onL1 != 5) FAILF("L1 has %llu batches, expected 5", (unsigned long long)onL1);
    if (!same3 || !same4) FAIL("the lost heights were not posted again unchanged");
    if (pending != 0) FAIL("pending left");
    if (rows != 5) FAILF("replica has %d rows", rows);
    PASS();
}

static void test_lost_beyond_retention_stops(void) {
    TEST("a reorg deeper than the kept batches: the writer stops with a reason, nothing posted past the gap");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", l1);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    for (int i = 1; i <= 40; i++) write_and_flush(w, i);    /* more than are kept */
    hpplite_l1_mock_truncate(l1, 2);
    write_and_flush(w, 41);
    hpplite_node_flush_batch(w);
    uint64_t onL1 = 0, total = 0; unsigned char lh[32];
    hpplite_l1_get_da_state(l1, &onL1, &total, lh);
    int state = w->state;
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (onL1 != 2) FAILF("L1 has %llu batches: posted past a gap it could not fill", (unsigned long long)onL1);
    if (state != HPPLITE_STATE_ERROR) FAIL("the writer did not stop");
    PASS();
}

/* A database belongs to the rollup its history is on. Opened against
** another one (a new wallet, another chain, a copied file), it must neither
** post its old batches there nor post past a gap: it stops and says so. */
static void test_other_rollup_refused(void) {
    TEST("a database opened against a rollup that is not its own: nothing posted, the writer stops");
    reset_dir();
    HppliteL1 *a = hpplite_l1_create_mock(), *b = hpplite_l1_create_mock();
    HppliteNode *w = open_node(DIR "/w.db", a);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE t(x)", NULL);
    for (int i = 1; i <= 3; i++) write_and_flush(w, i);
    hpplite_node_destroy(w);
    w = open_node(DIR "/w.db", b);                           /* the same file, another rollup */
    if (!w) FAIL("reopen");
    write_and_flush(w, 4);
    hpplite_node_flush_batch(w);
    uint64_t onB = 0, total = 0; unsigned char lh[32];
    hpplite_l1_get_da_state(b, &onB, &total, lh);
    int state = w->state;
    hpplite_node_destroy(w);
    w = open_node(DIR "/w.db", a);                           /* its own rollup: fine again */
    int ok = w && w->state != HPPLITE_STATE_ERROR;
    if (w) { hpplite_node_flush_batch(w); }
    int left = w ? hpplite_node_pending_count(w) : -1;
    if (w) hpplite_node_destroy(w);
    hpplite_l1_disconnect(a);
    hpplite_l1_disconnect(b);
    if (onB != 0) FAILF("%llu batches posted to a rollup that is not the database's", (unsigned long long)onB);
    if (state != HPPLITE_STATE_ERROR) FAIL("the writer did not stop");
    if (!ok || left != 0) FAIL("its own rollup did not take it back");
    PASS();
}

int main(void) {
    printf("=== HPPLite chain reorganisation (#49) ===\n");
    test_suite_init("reorg", PRIVKEY, DIR);
    test_lost_batches_reposted();
    test_lost_beyond_retention_stops();
    test_other_rollup_refused();
    return test_report();
}
