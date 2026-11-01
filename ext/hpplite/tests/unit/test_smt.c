/*
** The sparse Merkle tree of the version 2 state root (#9): shape and hash
** a pure function of the key set, whatever the order of operations.
*/
#include "test_lib.h"
#include "smt.h"
#include "sha256.h"

static void keyOf(uint64_t i, uint8_t out[32]) {
    SHA256_CTX h;
    uint8_t b[8];
    int k;
    for (k = 0; k < 8; k++) b[k] = (uint8_t)(i >> (8 * k));
    hpplite_sha256_init(&h);
    hpplite_sha256_update(&h, (const uint8_t *)"key", 3);
    hpplite_sha256_update(&h, b, 8);
    hpplite_sha256_final(&h, out);
}
static void valueOf(uint64_t i, int version, uint8_t out[32]) {
    SHA256_CTX h;
    uint8_t b[8];
    int k;
    for (k = 0; k < 8; k++) b[k] = (uint8_t)(i >> (8 * k));
    hpplite_sha256_init(&h);
    hpplite_sha256_update(&h, (const uint8_t *)"value", 5);
    hpplite_sha256_update(&h, b, 8);
    b[0] = (uint8_t)version;
    hpplite_sha256_update(&h, b, 1);
    hpplite_sha256_final(&h, out);
}

/* the root of keys lo..hi-1 at value version v, from the list builder */
static void expected_root(uint64_t lo, uint64_t hi, int v, const uint64_t *skip, size_t nSkip, uint8_t out[32]) {
    size_t n = 0, i;
    uint64_t k;
    HppliteSmtLeaf *a = malloc(sizeof(HppliteSmtLeaf) * (size_t)(hi - lo + 1));
    for (k = lo; k < hi; k++) {
        int skipped = 0;
        for (i = 0; i < nSkip; i++) if (skip[i] == k) skipped = 1;
        if (skipped) continue;
        keyOf(k, a[n].key);
        valueOf(k, v, a[n].hash);
        n++;
    }
    qsort(a, n, sizeof *a, hpplite_smt_leaf_cmp);
    hpplite_smt_build_root(a, n, out);
    free(a);
}

static void test_empty_and_single(void) {
    TEST("empty tree hashes to zeros; one key hashes to its leaf wherever it sits");
    HppliteSmtStore *s = hpplite_smt_mem_store();
    uint8_t root[32], zero[32] = {0}, k[32], v[32], leaf[32];
    if (hpplite_smt_root(s, "t", root) != 0 || memcmp(root, zero, 32) != 0) FAIL("empty root");
    keyOf(1, k); valueOf(1, 0, v);
    hpplite_smt_leaf_hash(k, v, leaf);
    if (hpplite_smt_set(s, "t", k, v) != 0) FAIL("set");
    hpplite_smt_root(s, "t", root);
    if (memcmp(root, leaf, 32) != 0) FAIL("a single key's root is its leaf hash");
    /* a second key pushes it down, clearing that key brings it back up */
    { uint8_t k2[32], v2[32]; keyOf(2, k2); valueOf(2, 0, v2);
      hpplite_smt_set(s, "t", k2, v2);
      hpplite_smt_root(s, "t", root);
      if (memcmp(root, leaf, 32) == 0) FAIL("two keys must not hash as one");
      hpplite_smt_set(s, "t", k2, NULL);
      hpplite_smt_root(s, "t", root);
      if (memcmp(root, leaf, 32) != 0) FAIL("after clearing the other key the root is the leaf again");
      if (hpplite_smt_mem_store_count(s) != 1) FAIL("one node left");
    }
    hpplite_smt_set(s, "t", k, NULL);
    hpplite_smt_root(s, "t", root);
    if (memcmp(root, zero, 32) != 0 || hpplite_smt_mem_store_count(s) != 0) FAIL("cleared to empty: zeros, no nodes");
    hpplite_smt_mem_store_free(s);
    PASS();
}

static void test_order_independence(void) {
    TEST("1000 keys in two orders, and from the sorted list: one root; node count bounded");
    HppliteSmtStore *a = hpplite_smt_mem_store(), *b = hpplite_smt_mem_store();
    uint8_t ra[32], rb[32], rx[32], k[32], v[32];
    uint64_t i;
    for (i = 0; i < 1000; i++) { keyOf(i, k); valueOf(i, 0, v); if (hpplite_smt_set(a, "t", k, v) != 0) FAIL("set a"); }
    for (i = 1000; i-- > 0;) { keyOf((i * 7919) % 1000, k); valueOf((i * 7919) % 1000, 0, v); hpplite_smt_set(b, "t", k, v); }
    hpplite_smt_root(a, "t", ra); hpplite_smt_root(b, "t", rb);
    expected_root(0, 1000, 0, NULL, 0, rx);
    if (memcmp(ra, rb, 32) != 0) FAIL("insertion order changed the root");
    if (memcmp(ra, rx, 32) != 0) FAIL("the list builder disagrees with the incremental tree");
    if (hpplite_smt_mem_store_count(a) != hpplite_smt_mem_store_count(b)) FAIL("shapes differ");
    if (hpplite_smt_mem_store_count(a) > 3000) FAIL("more than 3 nodes per key");
    printf("(%zu nodes for 1000 keys) ", hpplite_smt_mem_store_count(a));
    hpplite_smt_mem_store_free(a); hpplite_smt_mem_store_free(b);
    PASS();
}

static void test_updates_and_deletes(void) {
    TEST("values changed, half the keys cleared in random order: root equals a rebuild; all cleared: empty");
    HppliteSmtStore *s = hpplite_smt_mem_store();
    uint8_t root[32], rx[32], k[32], v[32], zero[32] = {0};
    uint64_t i, skip[500];
    size_t nSkip = 0;
    for (i = 0; i < 1000; i++) { keyOf(i, k); valueOf(i, 0, v); hpplite_smt_set(s, "t", k, v); }
    for (i = 0; i < 1000; i++) { keyOf(i, k); valueOf(i, 1, v); hpplite_smt_set(s, "t", k, v); }
    hpplite_smt_root(s, "t", root);
    expected_root(0, 1000, 1, NULL, 0, rx);
    if (memcmp(root, rx, 32) != 0) FAIL("after value updates");
    for (i = 0; i < 1000; i += 2) {
        uint64_t j = (i * 7919 + 13) % 1000;
        if (j % 2 == 0) j++;            /* 500 distinct odd keys, scrambled */
        keyOf(j, k);
        hpplite_smt_set(s, "t", k, NULL);
        skip[nSkip++] = j;
    }
    hpplite_smt_root(s, "t", root);
    expected_root(0, 1000, 1, skip, nSkip, rx);
    if (memcmp(root, rx, 32) != 0) FAIL("after clearing half");
    keyOf(123456, k);
    hpplite_smt_set(s, "t", k, NULL);          /* a key never set: no change */
    hpplite_smt_root(s, "t", root);
    if (memcmp(root, rx, 32) != 0) FAIL("clearing an absent key changed the root");
    for (i = 0; i < 1000; i += 2) { keyOf(i, k); hpplite_smt_set(s, "t", k, NULL); }
    hpplite_smt_root(s, "t", root);
    if (memcmp(root, zero, 32) != 0) FAIL("all cleared: zeros");
    if (hpplite_smt_mem_store_count(s) != 0) { printf("(%zu nodes left) ", hpplite_smt_mem_store_count(s)); FAIL("all cleared: no nodes left"); }
    hpplite_smt_mem_store_free(s);
    PASS();
}

static void test_deep_split(void) {
    TEST("two keys differing in the last bit: a 256-deep chain, collapsed again when one goes");
    HppliteSmtStore *s = hpplite_smt_mem_store();
    uint8_t k1[32] = {0}, k2[32] = {0}, v[32], root[32], rx[32], leaf[32];
    HppliteSmtLeaf a[2];
    k2[31] = 1;
    valueOf(1, 0, v);
    hpplite_smt_set(s, "t", k1, v);
    hpplite_smt_set(s, "t", k2, v);
    if (hpplite_smt_mem_store_count(s) != 256 + 2) { printf("(%zu nodes) ", hpplite_smt_mem_store_count(s)); FAIL("256 inner nodes and two leaves"); }
    memcpy(a[0].key, k1, 32); memcpy(a[0].hash, v, 32);
    memcpy(a[1].key, k2, 32); memcpy(a[1].hash, v, 32);
    hpplite_smt_build_root(a, 2, rx);
    hpplite_smt_root(s, "t", root);
    if (memcmp(root, rx, 32) != 0) FAIL("deep chain root");
    hpplite_smt_set(s, "t", k2, NULL);
    hpplite_smt_root(s, "t", root);
    hpplite_smt_leaf_hash(k1, v, leaf);
    if (memcmp(root, leaf, 32) != 0 || hpplite_smt_mem_store_count(s) != 1) FAIL("collapsed back to one leaf at the root");
    hpplite_smt_mem_store_free(s);
    PASS();
}

static void test_tables_apart(void) {
    TEST("trees are per table: the same keys in another table do not touch this one");
    HppliteSmtStore *s = hpplite_smt_mem_store();
    uint8_t k[32], v[32], r1[32], r2[32];
    keyOf(1, k); valueOf(1, 0, v);
    hpplite_smt_set(s, "a", k, v);
    hpplite_smt_root(s, "a", r1);
    valueOf(1, 1, v);
    hpplite_smt_set(s, "b", k, v);
    hpplite_smt_root(s, "a", r2);
    if (memcmp(r1, r2, 32) != 0) FAIL("table a changed");
    hpplite_smt_root(s, "b", r2);
    if (memcmp(r1, r2, 32) == 0) FAIL("table b has a different value");
    hpplite_smt_mem_store_free(s);
    PASS();
}

int main(void) {
    printf("=== HPPLite Sparse Merkle Tree Tests ===\n");
    test_suite_init("smt", NULL, NULL);
    test_empty_and_single();
    test_order_independence();
    test_updates_and_deletes();
    test_deep_split();
    test_tables_apart();
    return test_report();
}
