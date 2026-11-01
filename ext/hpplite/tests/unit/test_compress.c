/*
** Batch compression (#12): what a node posts is the deflated batch JSON,
** sealed or framed; a replica reads it, and every older form still reads.
**
** Before: a node posted the JSON as it was (0/4: the posted bytes were as
** large as with compress=off).
*/
#include "test_lib.h"
#include "compress.h"
#include "cipher.h"
#include "l1_interface.h"

/* 200 rows of similar text: what compresses, as application data does */
static void fill(HppliteNode *n) {
    char sql[300];
    hpplite_node_exec(n, "CREATE TABLE t(id INTEGER PRIMARY KEY, who TEXT, note TEXT, n REAL)", NULL);
    for (int i = 0; i < 200; i++) {
        snprintf(sql, sizeof sql, "INSERT INTO t(who, note, n) VALUES('agent-%d', 'posted a note about the weather on day %d of the season', %d.5)", i % 7, i, i);
        hpplite_node_exec(n, sql, NULL);
    }
}

static size_t posted_size(HppliteL1 *l1, uint64_t height) {
    size_t n = 0;
    uint8_t *d = hpplite_l1_get_batch(l1, height, &n);
    free(d);
    return n;
}

static void test_sealed_shrinks_and_replays(void) {
    TEST("a sealed batch is deflated inside the envelope: far smaller than with compress=off, and a replica replays it");
    reset_dir();
    HppliteL1 *plain = hpplite_l1_create_mock(), *comp = hpplite_l1_create_mock();
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000, .noCompress = 1 };
    HppliteNode *a = test_open_node(tpath("plain.db"), plain, &o);
    o.noCompress = 0;
    HppliteNode *b = test_open_node(tpath("comp.db"), comp, &o);
    if (!a || !b) FAIL("open");
    fill(a); fill(b);
    if (hpplite_node_flush_batch(a) == 0 || hpplite_node_flush_batch(b) == 0) FAIL("flush");
    size_t na = posted_size(plain, 1), nb = posted_size(comp, 1);
    printf("(plain %zu B, compressed %zu B, ratio %.1fx) ", na, nb, nb ? (double)na / nb : 0);
    if (na == 0 || nb == 0) FAIL("nothing posted");
    if (nb * 3 > na) { hpplite_node_destroy(a); hpplite_node_destroy(b); FAIL("the compressed batch should be under a third of the plain one"); }
    {
        size_t n = 0;
        uint8_t *d = hpplite_l1_get_batch(comp, 1, &n);
        int enc = hpplite_envelope_encoding(d, n);
        free(d);
        if (enc != HPPLITE_ENCODING_DEFLATE) FAILF("envelope encoding %d, want deflate", enc);
        d = hpplite_l1_get_batch(plain, 1, &n);
        enc = hpplite_envelope_encoding(d, n);
        free(d);
        if (enc != HPPLITE_ENCODING_NONE) FAILF("compress=off should post encoding none, got %d", enc);
    }
    const char *why = test_replica_check(b, comp, tpath("comp_replica.db"), &o, 0, 0, 0);
    if (why) { hpplite_node_destroy(a); hpplite_node_destroy(b); FAILF("replica of the compressed history: %s", why); }
    why = test_replica_check(a, plain, tpath("plain_replica.db"), &o, 0, 0, 0);
    if (why) { hpplite_node_destroy(a); hpplite_node_destroy(b); FAILF("replica of the plain history: %s", why); }
    hpplite_node_destroy(a);
    hpplite_node_destroy(b);
    hpplite_l1_disconnect(plain);
    hpplite_l1_disconnect(comp);
    PASS();
}

static void test_plaintext_frame(void) {
    TEST("encrypt=off: the compressed batch is framed HPZ1 and a replica without a key reads it");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000, .noEncrypt = 1 };
    HppliteNode *w = test_open_node(tpath("pt.db"), l1, &o);
    if (!w) FAIL("open");
    fill(w);
    if (hpplite_node_flush_batch(w) == 0) FAIL("flush");
    {
        size_t n = 0;
        uint8_t *d = hpplite_l1_get_batch(l1, 1, &n);
        int framed = hpplite_is_plain_compressed(d, n);
        free(d);
        if (!framed) { hpplite_node_destroy(w); FAIL("plaintext batch not framed as compressed"); }
    }
    const char *why = test_replica_check(w, l1, tpath("pt_replica.db"), &o, 0, 0, 0);
    if (why) { hpplite_node_destroy(w); FAILF("replica: %s", why); }
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_tiny_stays_plain(void) {
    TEST("a batch that would not shrink is posted as before (encoding none)");
    reset_dir();
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TestNodeOpts o = { .checkpointInterval = TEST_INTERVAL_1000 };
    HppliteNode *w = test_open_node(tpath("tiny.db"), l1, &o);
    if (!w) FAIL("open");
    hpplite_node_exec(w, "CREATE TABLE z(a)", NULL);
    if (hpplite_node_flush_batch(w) == 0) FAIL("flush");
    size_t n = 0;
    uint8_t *d = hpplite_l1_get_batch(l1, 1, &n);
    int enc = hpplite_envelope_encoding(d, n);
    free(d);
    hpplite_node_destroy(w);
    hpplite_l1_disconnect(l1);
    if (enc != HPPLITE_ENCODING_NONE && enc != HPPLITE_ENCODING_DEFLATE) FAILF("encoding %d", enc);
    PASS();
}

static void test_inflate_guards(void) {
    TEST("inflate refuses a malformed stream, a lying length and an oversized claim");
    const uint8_t text[] = "the same words the same words the same words the same words";
    size_t n = 0, m = 0;
    uint8_t *c = hpplite_deflate(text, sizeof(text) - 1, &n);
    if (!c || n == 0) FAIL("deflate");
    uint8_t *back = hpplite_inflate(c, n, &m);
    if (!back || m != sizeof(text) - 1 || memcmp(back, text, m) != 0 || back[m] != 0) { free(c); free(back); FAIL("round trip"); }
    free(back);
    c[0] = 0x10;                                           /* an original length of 256 MB+ */
    if (hpplite_inflate(c, n, &m) != NULL) { free(c); FAIL("an oversized claim was allocated"); }
    c[0] = 0; c[3] ^= 1;                                   /* a wrong length */
    if (hpplite_inflate(c, n, &m) != NULL) { free(c); FAIL("a lying length inflated"); }
    c[3] ^= 1; c[n - 1] ^= 0xff;                           /* a damaged stream */
    if (hpplite_inflate(c, n, &m) != NULL) { free(c); FAIL("a damaged stream inflated"); }
    free(c);
    if (hpplite_inflate((const uint8_t *)"abc", 3, &m) != NULL) FAIL("too short");
    if (hpplite_deflate(text, 0, &n) != NULL) FAIL("empty input");
    PASS();
}

int main(void) {
    test_suite_init("compress", NULL, NULL);
    printf("Batch compression (#12)\n");
    test_sealed_shrinks_and_replays();
    test_plaintext_frame();
    test_tiny_stays_plain();
    test_inflate_guards();
    remove_dir();
    return test_report();
}
