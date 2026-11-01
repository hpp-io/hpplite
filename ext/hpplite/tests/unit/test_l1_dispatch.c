/*
** L1 backends are a run-time choice (#35): one build carries the Ethereum
** client and the mock, and the endpoint picks one. Before this, each build
** had one backend: in a mock build hpplite_l1_connect("http://...") gave a
** mock, in a real build hpplite_l1_create_mock() did not exist.
*/

#include "l1_interface.h"
#include "sqlite3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run, tests_passed;
#define TEST(name) do { printf("  Testing: %s... ", name); tests_run++; } while (0)
#define PASS() do { printf("PASS\n"); tests_passed++; } while (0)
#define FAIL(msg) do { printf("FAIL: %s\n", msg); return; } while (0)

static const unsigned char KEY[32] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10,
    0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20
};
static const unsigned char INSTANCE[32] = { 0xaa };
#define CONTRACT "0x1111111111111111111111111111111111111111"

/* An http endpoint is the Ethereum client, whatever the build */
static void test_http_is_eth(void) {
    TEST("hpplite_l1_connect(\"http://...\") is the eth backend, not a mock");
    HppliteL1 *l1 = hpplite_l1_connect("http://127.0.0.1:9", CONTRACT);
    if (!l1) FAIL("connect returned NULL (connecting does not touch the network)");
    const char *name = hpplite_l1_backend_name(l1);
    if (!name || strcmp(name, "eth") != 0) { hpplite_l1_disconnect(l1); FAIL("backend name is not eth"); }
    /* the mock's test hooks do nothing on it */
    hpplite_l1_mock_advance_time(l1, 100);
    hpplite_l1_mock_set_fail_submit(l1, 1);
    if (hpplite_l1_mock_get_block(l1) != 0) { hpplite_l1_disconnect(l1); FAIL("an http endpoint gave a mock"); }
    if (hpplite_l1_mock_get_checkpoint_count(l1) != 0) { hpplite_l1_disconnect(l1); FAIL("mock checkpoint count on eth"); }
    char c[43], d[43];
    if (hpplite_l1_get_addresses(l1, c, d) != 0 || strcmp(c, CONTRACT) != 0) { hpplite_l1_disconnect(l1); FAIL("eth get_addresses"); }
    hpplite_l1_disconnect(l1);
    PASS();
}

/* "mock" is the mock, whatever the build: a lease and a batch round-trip */
static void test_mock_endpoint(void) {
    TEST("hpplite_l1_connect(\"mock\") is the mock backend and works end to end");
    HppliteL1 *l1 = hpplite_l1_connect("mock", NULL);
    if (!l1) FAIL("connect(mock) returned NULL");
    const char *name = hpplite_l1_backend_name(l1);
    if (!name || strcmp(name, "mock") != 0) { hpplite_l1_disconnect(l1); FAIL("backend name is not mock"); }
    if (hpplite_l1_set_privkey(l1, KEY) != 0) { hpplite_l1_disconnect(l1); FAIL("set_privkey"); }
    char *tx = hpplite_l1_claim_sequencer(l1, INSTANCE);
    if (!tx) { hpplite_l1_disconnect(l1); FAIL("claim"); }
    sqlite3_free(tx);
    if (!hpplite_l1_is_lease_active(l1)) { hpplite_l1_disconnect(l1); FAIL("lease not active after claim"); }
    const uint8_t data[] = "batch one";
    tx = hpplite_l1_submit_batch(l1, INSTANCE, 1, data, sizeof(data) - 1);
    if (!tx) { hpplite_l1_disconnect(l1); FAIL("submit_batch"); }
    sqlite3_free(tx);
    size_t n = 0;
    uint8_t *back = hpplite_l1_get_batch(l1, 1, &n);
    if (!back || n != sizeof(data) - 1 || memcmp(back, data, n) != 0) { free(back); hpplite_l1_disconnect(l1); FAIL("get_batch"); }
    free(back);
    uint64_t last = 0, total = 0;
    if (hpplite_l1_get_da_state(l1, &last, &total, NULL) != 0 || last != 1 || total != 1) { hpplite_l1_disconnect(l1); FAIL("da_state"); }
    /* the mock's hooks work on it */
    uint64_t t0 = hpplite_l1_mock_get_timestamp(l1);
    hpplite_l1_mock_advance_time(l1, 100);
    if (hpplite_l1_mock_get_timestamp(l1) != t0 + 100) { hpplite_l1_disconnect(l1); FAIL("mock advance_time"); }
    hpplite_l1_disconnect(l1);

    l1 = hpplite_l1_create_mock();
    if (!l1 || strcmp(hpplite_l1_backend_name(l1), "mock") != 0) { hpplite_l1_disconnect(l1); FAIL("create_mock is the mock backend"); }
    hpplite_l1_disconnect(l1);
    PASS();
}

/* The calls made before there is a connection go by the endpoint too */
static void test_factory_dispatch(void) {
    TEST("factory calls dispatch on the endpoint: mock has no factory");
    uint8_t rollup[20];
    if (hpplite_l1_connect_factory("mock", CONTRACT, KEY) != NULL) FAIL("mock connect_factory should be NULL");
    if (hpplite_l1_factory_get_my_rollup("mock", CONTRACT, KEY, rollup) != 0) FAIL("mock factory_get_my_rollup should be 0");
    /* eth: an unreachable endpoint fails, it does not pretend */
    if (hpplite_l1_factory_get_my_rollup("http://127.0.0.1:9", CONTRACT, KEY, rollup) != -1) FAIL("eth factory on a dead endpoint should be -1");
    PASS();
}

static void test_shared_chain_id(void) {
    TEST("the default chain id is one setting for every backend");
    uint64_t before = hpplite_l1_default_chain_id();
    hpplite_l1_set_default_chain_id(31337);
    if (hpplite_l1_default_chain_id() != 31337) FAIL("set_default_chain_id");
    hpplite_l1_set_default_chain_id(before);
    PASS();
}

static void test_null_handle(void) {
    TEST("every call tolerates a NULL connection");
    char c[43], d[43];
    uint64_t a, b;
    if (hpplite_l1_backend_name(NULL) != NULL) FAIL("name of NULL");
    if (hpplite_l1_get_state(NULL) != NULL) FAIL("get_state(NULL)");
    if (hpplite_l1_is_lease_active(NULL) != 0) FAIL("is_lease_active(NULL)");
    if (hpplite_l1_get_addresses(NULL, c, d) != -1) FAIL("get_addresses(NULL)");
    if (hpplite_l1_get_da_state(NULL, &a, &b, NULL) != -1) FAIL("get_da_state(NULL)");
    if (hpplite_l1_submit_batch(NULL, INSTANCE, 1, (const uint8_t *)"x", 1) != NULL) FAIL("submit_batch(NULL)");
    hpplite_l1_disconnect(NULL);
    hpplite_l1_mock_advance_time(NULL, 1);
    PASS();
}

int main(void) {
    printf("L1 dispatch (#35)\n");
    test_http_is_eth();
    test_mock_endpoint();
    test_factory_dispatch();
    test_shared_chain_id();
    test_null_handle();
    printf("\nResults: %d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
