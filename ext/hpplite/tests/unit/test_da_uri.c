/*
** HPPLite DA URI Test
**
** Tests DA URI parsing, building, and network alias resolution.
*/

#include "da_uri.h"
#include "l1_interface.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_passed = 0;

#define TEST(name) printf("Test: %s\n", name); tests_run++
#define PASS() printf("  PASS\n"); tests_passed++
#define FAIL(msg) printf("  FAIL: %s\n", msg); return

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

static void test_scheme_parse(void) {
    TEST("Scheme parsing");

    if (hpplite_da_scheme_parse("hppda") != HPPLITE_DA_HPPDA) {
        FAIL("hppda not recognized");
    }
    if (hpplite_da_scheme_parse("ipfs") != HPPLITE_DA_IPFS) {
        FAIL("ipfs not recognized");
    }
    if (hpplite_da_scheme_parse("file") != HPPLITE_DA_FILE) {
        FAIL("file not recognized");
    }
    if (hpplite_da_scheme_parse("http") != HPPLITE_DA_HTTP) {
        FAIL("http not recognized");
    }
    if (hpplite_da_scheme_parse("invalid") != HPPLITE_DA_UNKNOWN) {
        FAIL("invalid should return unknown");
    }

    PASS();
}

static void test_network_alias(void) {
    TEST("Network alias resolution");

    /* Test numeric chain ID */
    if (hpplite_da_resolve_alias("181228") != 181228) {
        FAIL("numeric chain ID not parsed");
    }

    /* Test hpp-sepolia alias */
    if (hpplite_da_resolve_alias("hpp-sepolia") != 181228) {
        FAIL("hpp-sepolia alias not resolved");
    }

    /* Test unknown alias */
    if (hpplite_da_resolve_alias("unknown-network") != 0) {
        FAIL("unknown alias should return 0");
    }

    /* Test RPC URL lookup */
    const char *rpc = hpplite_da_get_rpc_url(181228);
    if (!rpc || strcmp(rpc, "https://sepolia.hpp.io") != 0) {
        FAIL("RPC URL lookup failed");
    }

    /* Test factory address lookup */
    const char *factory = hpplite_da_get_factory(181228);
    if (!factory) {
        FAIL("Factory lookup returned NULL for hpp-sepolia");
    }
    if (strncmp(factory, "0x", 2) != 0) {
        FAIL("Factory address should start with 0x");
    }
    printf("  hpp-sepolia factory: %s\n", factory);

    /* Test network lookup */
    const HppliteNetworkAlias *network = hpplite_da_lookup_network("hpp-sepolia");
    if (!network) {
        FAIL("Network lookup returned NULL");
    }
    if (network->chainId != 181228) {
        FAIL("Network lookup wrong chainId");
    }
    if (!network->factory) {
        FAIL("Network should have factory");
    }

    PASS();
}

static void test_uri_parse_hppda(void) {
    TEST("Parse hppda:// URI with chain ID");

    const char *uri = "hppda://181228/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/42";
    HppliteDAUri *parsed = hpplite_da_uri_parse(uri);

    if (!parsed) {
        FAIL("Parse returned NULL");
    }
    if (parsed->scheme != HPPLITE_DA_HPPDA) {
        hpplite_da_uri_free(parsed);
        FAIL("Wrong scheme");
    }
    if (parsed->chainId != 181228) {
        hpplite_da_uri_free(parsed);
        FAIL("Wrong chain ID");
    }
    if (parsed->heightFrom != 42 || parsed->heightTo != 42) {
        hpplite_da_uri_free(parsed);
        FAIL("Wrong height");
    }

    printf("  Parsed: scheme=%s chainId=%llu height=%llu contract=0x",
           hpplite_da_scheme_str(parsed->scheme),
           (unsigned long long)parsed->chainId,
           (unsigned long long)parsed->heightFrom);
    print_hex(parsed->contract, 20);
    printf("\n");

    hpplite_da_uri_free(parsed);
    PASS();
}

static void test_uri_parse_alias(void) {
    TEST("Parse hppda:// URI with network alias");

    const char *uri = "hppda://hpp-sepolia/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/100";
    HppliteDAUri *parsed = hpplite_da_uri_parse(uri);

    if (!parsed) {
        FAIL("Parse returned NULL");
    }
    if (parsed->chainId != 181228) {
        hpplite_da_uri_free(parsed);
        FAIL("Alias not resolved to chain ID");
    }
    if (parsed->heightFrom != 100) {
        hpplite_da_uri_free(parsed);
        FAIL("Wrong height");
    }

    hpplite_da_uri_free(parsed);
    PASS();
}

static void test_uri_parse_range(void) {
    TEST("Parse hppda:// URI with height range");

    const char *uri = "hppda://181228/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/1-10";
    HppliteDAUri *parsed = hpplite_da_uri_parse(uri);

    if (!parsed) {
        FAIL("Parse returned NULL");
    }
    if (parsed->heightFrom != 1 || parsed->heightTo != 10) {
        printf("  Got: from=%llu to=%llu\n",
               (unsigned long long)parsed->heightFrom,
               (unsigned long long)parsed->heightTo);
        hpplite_da_uri_free(parsed);
        FAIL("Wrong height range");
    }

    hpplite_da_uri_free(parsed);
    PASS();
}

static void test_uri_build(void) {
    TEST("Build hppda:// URI");

    unsigned char contract[20] = {
        0x2c, 0xd2, 0x7d, 0x02, 0xc3, 0xb3, 0x6c, 0x89, 0x26, 0xb8,
        0x49, 0xbc, 0x53, 0x74, 0x5f, 0xab, 0x66, 0x15, 0x72, 0xdc
    };

    char *uri = hpplite_da_uri_build(HPPLITE_DA_HPPDA, 181228, contract, 42, 42);
    if (!uri) {
        FAIL("Build returned NULL");
    }

    printf("  Built: %s\n", uri);

    /* Verify it can be parsed back */
    HppliteDAUri *parsed = hpplite_da_uri_parse(uri);
    if (!parsed) {
        free(uri);
        FAIL("Could not parse built URI");
    }
    if (parsed->chainId != 181228 || parsed->heightFrom != 42) {
        hpplite_da_uri_free(parsed);
        free(uri);
        FAIL("Round-trip failed");
    }

    hpplite_da_uri_free(parsed);
    free(uri);
    PASS();
}

static void test_uri_from_config(void) {
    TEST("Build URI from system config");

    HppliteSystemConfig config = {
        .daScheme = "hppda",
        .daContract = {
            0x2c, 0xd2, 0x7d, 0x02, 0xc3, 0xb3, 0x6c, 0x89, 0x26, 0xb8,
            0x49, 0xbc, 0x53, 0x74, 0x5f, 0xab, 0x66, 0x15, 0x72, 0xdc
        },
        .batchSizeLimit = 131072,
        .version = 2,
        .chainId = 181228
    };

    char *uri = hpplite_da_uri_from_config(&config, 1, 100);
    if (!uri) {
        FAIL("Build from config returned NULL");
    }

    printf("  Built: %s\n", uri);

    /* Should contain range */
    if (strstr(uri, "1-100") == NULL) {
        free(uri);
        FAIL("Range not in URI");
    }

    free(uri);
    PASS();
}

static void test_uri_parse_file(void) {
    TEST("Parse file:// URI");

    const char *uri = "file:///path/to/batch.json";
    HppliteDAUri *parsed = hpplite_da_uri_parse(uri);

    if (!parsed) {
        FAIL("Parse returned NULL");
    }
    if (parsed->scheme != HPPLITE_DA_FILE) {
        hpplite_da_uri_free(parsed);
        FAIL("Wrong scheme");
    }
    if (!parsed->path || strcmp(parsed->path, "/path/to/batch.json") != 0) {
        hpplite_da_uri_free(parsed);
        FAIL("Wrong path");
    }

    printf("  Parsed: scheme=%s path=%s\n",
           hpplite_da_scheme_str(parsed->scheme),
           parsed->path);

    hpplite_da_uri_free(parsed);
    PASS();
}


/* The chain's own cadence: HPP anchors every 30 minutes, so l1=hpp-sepolia
** sets the batch interval to 30 minutes unless the URI says interval= (#162);
** a local chain keeps the default */
static void test_alias_interval(void) {
    TEST("Chain alias sets the batch interval");
    struct { const char *uri; int ms; } cases[] = {
        {"hpplite://x?hpplite=1&l1=hpp-sepolia&privkey=0x" "0000000000000000000000000000000000000000000000000000000000000001", 30 * 60 * 1000},
        {"hpplite://x?hpplite=1&l1=hpp-sepolia&interval=5s&privkey=0x" "0000000000000000000000000000000000000000000000000000000000000001", 5000},
        {"hpplite://x?hpplite=1&l1=hpp-mainnet", 30 * 60 * 1000},
        {"hpplite://x?hpplite=1&l1=local", 5000},
        {"hpplite://x?hpplite=1", 5000},
    };
    size_t i;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        HppliteConfig *cfg = hpplite_config_load(cases[i].uri);
        int ms = cfg ? cfg->batchIntervalMs : -1;
        if (cfg) hpplite_config_free(cfg);
        if (ms != cases[i].ms) {
            printf("  %s: interval %d ms, expected %d\n", cases[i].uri, ms, cases[i].ms);
            FAIL("interval");
        }
    }
    PASS();
}

/* store=on and relay=on take the network's canonical operators (#192): HPP Sepolia's; a URL given stays; a network
** with none is an error, not a node that quietly posts on chain */
static void test_operators_on(void) {
    TEST("store=on and relay=on: the network's canonical operators");
    HppliteConfig *cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=on&relay=on");
    int ok = cfg && hpplite_config_resolve_operators(cfg) == 0 && cfg->storeUrl && cfg->relayUrl
             && strcmp(cfg->storeUrl, "https://store.sepolia.hpplite.hpp.io") == 0 && strcmp(cfg->relayUrl, "https://relay.sepolia.hpplite.hpp.io") == 0;
    if (cfg) { printf("  hpp-sepolia: store %s, relay %s\n", cfg->storeUrl ? cfg->storeUrl : "-", cfg->relayUrl ? cfg->relayUrl : "-"); hpplite_config_free(cfg); }
    if (!ok) { FAIL("hpp-sepolia's store and relay"); }
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=http://127.0.0.1:9");
    ok = cfg && hpplite_config_resolve_operators(cfg) == 0 && strcmp(cfg->storeUrl, "http://127.0.0.1:9") == 0;
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { FAIL("a URL given stays"); }
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-mainnet&store=on");
    ok = cfg && hpplite_config_resolve_operators(cfg) != 0 && hpplite_config_error() && strstr(hpplite_config_error(), "no canonical store");
    printf("  hpp-mainnet: %s\n", hpplite_config_error() ? hpplite_config_error() : "(no error)");
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { FAIL("a network without a canonical store: an error"); }
    PASS();
}


/* keep= and snapshot= together must fit the snapshots a manifest can name (#231) */
static void test_keep_snapshot_cap(void) {
    TEST("keep= with snapshot=: refused at open when they would keep more snapshots than a manifest names (64)");
    HppliteConfig *cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=on&anchor=30m&snapshot=1h&keep=30d");
    int ok = cfg && hpplite_config_resolve_operators(cfg) != 0 && hpplite_config_error() && strstr(hpplite_config_error(), "720 snapshots");
    printf("  anchor=30m snapshot=1h keep=30d: %s\n", hpplite_config_error() ? hpplite_config_error() : "(no error)");
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { FAIL("720 snapshots would be kept: the open must be refused and say so"); }
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=on&anchor=30m&snapshot=1h&keep=48h");
    ok = cfg && hpplite_config_resolve_operators(cfg) == 0;
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { printf("  snapshot=1h keep=48h: %s\n", hpplite_config_error() ? hpplite_config_error() : "(no error)"); FAIL("48 snapshots fit"); }
    /* a snapshot is made at an anchor: snapshot=1ms with anchor=1h is one an hour; 48 h of them fit */
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=on&anchor=1h&snapshot=1ms&keep=48h");
    ok = cfg && hpplite_config_resolve_operators(cfg) == 0;
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { printf("  anchor=1h snapshot=1ms keep=48h: %s\n", hpplite_config_error() ? hpplite_config_error() : "(no error)"); FAIL("the cadence is the anchor's: 48 fit"); }
    /* anchors by hand (anchor=0): the cadence is not known here; the anchor refuses a snapshot when the list is full */
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=on&anchor=0&snapshot=1ms&keep=3s");
    ok = cfg && hpplite_config_resolve_operators(cfg) == 0;
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { printf("  anchor=0 snapshot=1ms keep=3s: %s\n", hpplite_config_error() ? hpplite_config_error() : "(no error)"); FAIL("anchors by hand: not refused at open"); }
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=on&snapshot=1h");
    ok = cfg && hpplite_config_resolve_operators(cfg) == 0;
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { FAIL("snapshot= without keep= (keep=all) is not capped"); }
    PASS();
}

/* #251: the relay's x402 batch-settlement channels are gone - the relay is paid per call (exact); relaydeposit= refuses
** the open. relay=on with no relaycap= pays up to a default cap: relay=on alone could never pay before (cap 0) */
#ifndef HPPLITE_DEFAULT_RELAY_CAP
#define HPPLITE_DEFAULT_RELAY_CAP 1000000
#endif
static void test_relay_exact_only(void) {
    TEST("relaydeposit= refuses the open (channels removed, #251); relay=on gets a default relaycap");
    HppliteConfig *cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&relay=on&relaydeposit=500000");
    int refused = !cfg || hpplite_config_error() != NULL;
    const char *why = hpplite_config_error();
    if (cfg) hpplite_config_free(cfg);
    if (!refused || !why || !strstr(why, "relaydeposit")) { printf("  relaydeposit=500000: %s\n", why ? why : "(no error)"); FAIL("relaydeposit= should refuse the open"); }
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&relay=on");
    int ok = cfg && hpplite_config_resolve_operators(cfg) == 0 && cfg->relayCap == HPPLITE_DEFAULT_RELAY_CAP;
    if (cfg) printf("  relay=on: relaycap %llu\n", (unsigned long long)cfg->relayCap);
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { FAIL("relay=on without relaycap= should take the default cap"); }
    setenv("HPPLITE_RELAY_CAP", "7", 1);
    cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&relay=on");
    unsetenv("HPPLITE_RELAY_CAP");
    ok = cfg && hpplite_config_resolve_operators(cfg) == 0 && cfg->relayCap == 7;
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { FAIL("HPPLITE_RELAY_CAP given keeps its value"); }
    PASS();
}

/* #254: the money knobs are the deployment's, not the connection string's. Each in a URI refuses the open naming
** its environment variable; the variable is honoured; storefallback gets the twin it never had */
static void test_money_knobs_env_only(void) {
    TEST("storecap=, storepermit=, spendcap=, lowbalance=, relaycap=, storefallback= refuse the open naming their variable; the environment is read (#254)");
    static const char *knobs[][2] = {
        { "storecap=5", "HPPLITE_STORE_CAP" }, { "storepermit=5", "HPPLITE_STORE_PERMIT" }, { "spendcap=5/hour", "HPPLITE_SPEND_CAP" },
        { "lowbalance=5", "HPPLITE_LOW_BALANCE" }, { "relaycap=5", "HPPLITE_RELAY_CAP" }, { "storefallback=off", "HPPLITE_STORE_FALLBACK" },
    };
    for (int i = 0; i < 6; i++) {
        char uri[200];
        snprintf(uri, sizeof uri, "hpplite://x?hpplite=1&l1=hpp-sepolia&store=on&%s", knobs[i][0]);
        HppliteConfig *cfg = hpplite_config_load(uri);
        const char *why = hpplite_config_error();
        int refused = why && strstr(why, knobs[i][1]);
        printf("\n  %s: %s", knobs[i][0], why ? why : "(no error)");
        if (cfg) hpplite_config_free(cfg);
        if (!refused) { printf("\n"); FAIL("a money knob in a URI should refuse the open naming its environment variable"); }
    }
    setenv("HPPLITE_STORE_PERMIT", "5", 1);
    setenv("HPPLITE_SPEND_CAP", "4500/hour", 1);
    setenv("HPPLITE_STORE_FALLBACK", "off", 1);
    HppliteConfig *cfg = hpplite_config_load("hpplite://x?hpplite=1&l1=hpp-sepolia&store=on");
    unsetenv("HPPLITE_STORE_PERMIT"); unsetenv("HPPLITE_SPEND_CAP"); unsetenv("HPPLITE_STORE_FALLBACK");
    int ok = cfg && hpplite_config_error() == NULL && cfg->storePermit == 5 && cfg->storePermitSet
             && cfg->spendCap == 4500 && cfg->spendWindow == 3600 && cfg->storeFallback == 0;
    if (cfg) printf("\n  environment: permit %llu (set %d), spendcap %llu/%llu, fallback %d\n  ", (unsigned long long)cfg->storePermit, cfg->storePermitSet,
                    (unsigned long long)cfg->spendCap, (unsigned long long)cfg->spendWindow, cfg->storeFallback);
    if (cfg) hpplite_config_free(cfg);
    if (!ok) { FAIL("HPPLITE_STORE_PERMIT, HPPLITE_SPEND_CAP and HPPLITE_STORE_FALLBACK should be read from the environment"); }
    PASS();
}

int main(void) {
    printf("test_da_uri: DA URI Scheme Tests\n");
    printf("================================\n\n");

    test_scheme_parse();
    test_network_alias();
    test_uri_parse_hppda();
    test_uri_parse_alias();
    test_uri_parse_range();
    test_uri_build();
    test_uri_from_config();
    test_uri_parse_file();
    test_alias_interval();
    test_operators_on();
    test_keep_snapshot_cap();
    test_relay_exact_only();
    test_money_knobs_env_only();

    printf("\n================================\n");
    printf("%d/%d tests passed\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
