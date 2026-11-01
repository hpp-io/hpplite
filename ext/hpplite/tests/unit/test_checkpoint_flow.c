/*
** Checkpoint flow (singleton sequencer)
**
** The state root is computed only on the batch that closes a checkpoint
** interval, and the flush loop submits the checkpoint right after it.
*/

#include "test_lib.h"

static const unsigned char SEQ_PRIVKEY[32] = {
    0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10,
    0x11,0x12,0x13,0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c,0x1d,0x1e,0x1f,0x20
};

#define DIR "/tmp/hpplite_test_checkpoint"

/* a fresh file each time, in the local throw-away mode (#38) with the mock
** attached but not synced from; checkpointInterval 0: none given */
static HppliteNode *open_node(HppliteL1 *l1, int checkpointInterval) {
    TestNodeOpts o = { .checkpointInterval = checkpointInterval ? checkpointInterval : TEST_INTERVAL_NODE, .localOnly = 1, .noSync = 1 };
    reset_dir();
    return test_open_node(DIR "/test.db", l1, &o);
}

static HppliteBatch *l1_batch(HppliteNode *node, uint64_t h) {
    return hpplite_node_fetch_batch(node, h);
}

/* Roots appear only on interval-closing batches; the checkpoint follows automatically */
static void test_checkpoint_auto(void) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *node = open_node(l1, 3);
    unsigned char root[32];
    TEST("root on every 3rd batch, checkpoint submitted automatically");
    if (!node) FAIL("open");
    for (int i = 0; i < 3; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "CREATE TABLE t%d (x INTEGER)", i);
        hpplite_node_exec(node, sql, NULL);
        if (hpplite_node_flush_batch(node) != (uint64_t)(i + 1)) FAIL("flush");
        if (i < 2 && hpplite_l1_mock_get_checkpoint_count(l1) != 0) FAIL("checkpoint too early");
    }
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) FAIL("checkpoint should have been submitted after batch 3");
    if (hpplite_node_should_checkpoint(node)) FAIL("interval should have been reset");
    if (hpplite_node_submit_checkpoint(node) != NULL) FAIL("nothing to checkpoint manually");

    for (uint64_t h = 1; h <= 3; h++) {
        HppliteBatch *b = l1_batch(node, h);
        if (!b) FAIL("batch missing");
        if (h < 3 && b->hasRoot) FAIL("non-closing batch must not carry a root");
        if (h == 3 && !b->hasRoot) FAIL("closing batch must carry the root");
        if (h == 3) {
            hpplite_node_get_state_root(node, root);
            if (memcmp(b->postStateRoot, root, 32) != 0) FAIL("batch root != content root");
        }
        hpplite_batch_free(b);
    }
    const HppliteCheckpoint *cp = hpplite_l1_mock_get_last_checkpoint(l1);
    if (!cp || cp->fromHeight != 1 || cp->toHeight != 3) FAIL("checkpoint range");
    if (memcmp(cp->postStateRoot, root, 32) != 0) FAIL("checkpoint root != content root");
    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* Consecutive intervals chain: pre root of the second = post root of the first */
static void test_checkpoint_chain(void) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *node = open_node(l1, 2);
    unsigned char first[32];
    TEST("consecutive checkpoints chain their roots");
    if (!node) FAIL("open");
    hpplite_node_exec(node, "CREATE TABLE t1 (x)", NULL); hpplite_node_flush_batch(node);
    hpplite_node_exec(node, "INSERT INTO t1 VALUES (1)", NULL); hpplite_node_flush_batch(node);
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) FAIL("first checkpoint");
    memcpy(first, hpplite_l1_mock_get_last_checkpoint(l1)->postStateRoot, 32);

    hpplite_node_exec(node, "INSERT INTO t1 VALUES (2)", NULL); hpplite_node_flush_batch(node);
    hpplite_node_exec(node, "INSERT INTO t1 VALUES (3)", NULL); hpplite_node_flush_batch(node);
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 2) FAIL("second checkpoint");
    const HppliteCheckpoint *cp = hpplite_l1_mock_get_last_checkpoint(l1);
    if (cp->fromHeight != 3 || cp->toHeight != 4) FAIL("second checkpoint range");
    if (memcmp(cp->preStateRoot, first, 32) != 0) FAIL("second checkpoint must start from the first's root");
    if (memcmp(cp->preStateRoot, cp->postStateRoot, 32) == 0) FAIL("roots should differ");
    if (node->lastCheckpointHeight != 4) FAIL("lastCheckpointHeight");
    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* Interval of 1: every batch carries a root that matches the content */
static void test_checkpoint_every_batch(void) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    HppliteNode *node = open_node(l1, 1);
    unsigned char r1[32], r2[32];
    TEST("interval 1: each batch is a checkpoint with the content root");
    if (!node) FAIL("open");
    hpplite_node_exec(node, "CREATE TABLE t (x INTEGER)", NULL);
    hpplite_node_flush_batch(node);
    hpplite_node_get_state_root(node, r1);
    if (memcmp(hpplite_l1_mock_get_last_checkpoint(l1)->postStateRoot, r1, 32) != 0) FAIL("cp1 root");
    hpplite_node_exec(node, "INSERT INTO t VALUES (42)", NULL);
    hpplite_node_flush_batch(node);
    hpplite_node_get_state_root(node, r2);
    if (memcmp(r1, r2, 32) == 0) FAIL("roots should differ after INSERT");
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 2) FAIL("two checkpoints");
    if (memcmp(hpplite_l1_mock_get_last_checkpoint(l1)->postStateRoot, r2, 32) != 0) FAIL("cp2 root");
    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    PASS();
}

/* No L1: the interval is tracked, nothing is submitted, roots still land on closing batches */
static void test_checkpoint_without_l1(void) {
    TestNodeOpts local = { .nodeId = "local", .checkpointInterval = 2, .localOnly = 1 };
    HppliteNode *node;
    TEST("without L1 the interval is tracked and manual checkpoint is a no-op");
    reset_dir();
    node = test_open_node(DIR "/local.db", NULL, &local);
    if (!node) FAIL("open");
    hpplite_node_exec(node, "CREATE TABLE t (x)", NULL); hpplite_node_flush_batch(node);
    hpplite_node_exec(node, "INSERT INTO t VALUES (1)", NULL); hpplite_node_flush_batch(node);
    if (!hpplite_node_should_checkpoint(node)) FAIL("should want a checkpoint");
    if (hpplite_node_submit_checkpoint(node) != NULL) FAIL("no L1, nothing to submit");
    hpplite_node_destroy(node);
    PASS();
}

/* The contract's checkpointInterval is the configuration of record (#44):
** a node that sets none takes it; one that sets its own writes it back */
static void test_interval_from_contract(void) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    char *tx;
    TEST("no checkpoint= given: the contract's interval is used");
    hpplite_l1_set_privkey(l1, SEQ_PRIVKEY);                 /* the owner */
    tx = hpplite_l1_set_checkpoint_interval(l1, 3);
    if (!tx) FAIL("premise: the owner sets the contract's interval");
    sqlite3_free(tx);
    HppliteNode *node = open_node(l1, 0);                    /* 0: none given */
    if (!node) FAIL("open");
    if (node->config.checkpointInterval != 3) FAILF("node uses %d, the contract says 3", node->config.checkpointInterval);
    for (int i = 0; i < 3; i++) {
        char sql[64];
        snprintf(sql, sizeof(sql), "CREATE TABLE t%d (x INTEGER)", i);
        hpplite_node_exec(node, sql, NULL);
        if (hpplite_node_flush_batch(node) != (uint64_t)(i + 1)) FAIL("flush");
    }
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 1) FAIL("a checkpoint after 3 batches, as the contract says");
    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    PASS();
}

static void test_interval_written_back(void) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    TEST("checkpoint=2 given: written to the contract by its owner, so on-chain config and reality agree");
    HppliteNode *node = open_node(l1, 2);
    if (!node) FAIL("open");
    HppliteL1State *st = hpplite_l1_get_state(l1);
    uint64_t onChain = st ? st->checkpointInterval : 0;
    hpplite_l1_state_free(st);
    if (onChain != 2) { hpplite_node_destroy(node); FAILF("the contract says %llu, the node uses 2", (unsigned long long)onChain); }
    hpplite_node_destroy(node);
    hpplite_l1_disconnect(l1);
    PASS();
}

int main(void) {
    printf("=== Checkpoint Flow Tests ===\n");
    test_suite_init("test-seq", SEQ_PRIVKEY, DIR);
    test_checkpoint_auto();
    test_checkpoint_chain();
    test_checkpoint_every_batch();
    test_checkpoint_without_l1();
    test_interval_from_contract();
    test_interval_written_back();
    remove_dir();
    return test_report();
}
