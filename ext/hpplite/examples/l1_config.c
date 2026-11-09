/*
** HPPLite Example: L1 Configuration
**
** Demonstrates:
** - Transparent SQLite API with ?hpplite=on URI parameter
** - Using network aliases (l1=hpp-sepolia)
** - Reading system config from L1 contract
** - Building DA URIs
**
** No registration needed - HPPLite is built into SQLite!
**
** Build:
**   gcc -I.. -I../../build -o l1_config l1_config.c \
**       ../build/libhpplite.a ../build/libsqlite3.a \
**       -L/opt/homebrew/lib -lsecp256k1 -lcurl -lzmq -lpthread
*/

#include "hpplite.h"
#include "l1_interface.h"
#include "da_uri.h"
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* HPPLiteDA v2 on HPP Sepolia */
#include "test_util.h"
#define DATA_DIR "/tmp/hpplite_l1"

static void print_hex(const unsigned char *data, int len) {
    for (int i = 0; i < len; i++) printf("%02x", data[i]);
}

int main(void) {
    printf("HPPLite L1 Configuration Example\n");
    printf("==================================\n\n");

    /* Clean up */
    system("rm -rf " DATA_DIR " && mkdir -p " DATA_DIR);

    /*
     * Open database with standard SQLite API.
     *
     * Using l1=hpp-sepolia automatically sets:
     *   - chainId = 181228
     *   - rpcUrl = https://sepolia.hpp.io
     *
     * You can also use explicit chainid= and rpc= parameters.
     */
    /* A fresh funded wallet; the factory creates its rollup on first open */
    char privkey[128], address[64];
    if (!test_util_has_cast()) {
        printf("ERROR: 'cast' not found. Install foundry: https://getfoundry.sh\n");
        return 1;
    }
    if (test_util_create_funded_wallet(privkey, sizeof(privkey), address, sizeof(address),
                                        HPPLITE_DEFAULT_RPC, "0.001ether") != 0) {
        return 1;
    }
    printf("Wallet: %s\n", address);

    printf("Opening database with L1 configuration...\n");
    char uri[1024];
    snprintf(uri, sizeof(uri),
        "file:" DATA_DIR "/state.db?hpplite=on&l1=hpp-sepolia&privkey=%s&datadir=" DATA_DIR,
        privkey);
    sqlite3 *db;
    int rc = sqlite3_open_v2(uri, &db,
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI, NULL);
    if (rc != SQLITE_OK) {
        printf("ERROR: Failed to open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    /* Get the configuration */
    HppliteConfig *cfg = hpplite_get_config(db);
    if (cfg) {
        printf("\nLoaded Configuration:\n");
        printf("  Role:     %s\n", cfg->roleVerify ? "verify" : "sequencer");   /* role= takes verify only (#248) */
        printf("  Chain ID: %llu\n", (unsigned long long)cfg->chainId);
        printf("  RPC URL:  %s\n", cfg->rpcUrl);
        printf("  Contract: 0x");
        if (cfg->hasContract) {
            print_hex(cfg->contract, 20);
        } else {
            printf("(not set)");
        }
        printf("\n");
        printf("  Data Dir: %s\n", cfg->dataDir);
    }

    /* Demonstrate network alias resolution */
    printf("\nNetwork Alias Resolution:\n");
    printf("  hpp-sepolia -> chain ID %llu\n",
           (unsigned long long)hpplite_da_resolve_alias("hpp-sepolia"));
    printf("  hpp-mainnet -> chain ID %llu\n",
           (unsigned long long)hpplite_da_resolve_alias("hpp-mainnet"));

    const char *rpc = hpplite_da_get_rpc_url(181228);
    printf("  Chain 181228 -> RPC %s\n", rpc ? rpc : "(not found)");

    /* Get L1 connection (auto-connected from URI params) */
    printf("\nL1 Connection (auto-connected from URI)...\n");
    HppliteL1 *l1 = hpplite_get_l1(db);
    if (!l1) {
        printf("ERROR: L1 not connected\n");
        return 1;
    }
    if (l1) {
        printf("  L1 connected successfully!\n");

        HppliteSystemConfig *syscfg = hpplite_l1_get_system_config(l1);
        if (syscfg) {
            printf("\nSystem Configuration (from L1 contract):\n");
            printf("  DA Scheme:        %s\n", syscfg->daScheme);
            printf("  DA Contract:      0x");
            print_hex(syscfg->daContract, 20);
            printf("\n");
            printf("  Batch Size Limit: %llu bytes\n",
                   (unsigned long long)syscfg->batchSizeLimit);
            printf("  Version:          %llu\n", (unsigned long long)syscfg->version);

            /* Build example DA URIs */
            printf("\nExample DA URIs:\n");
            char *uri1 = hpplite_da_uri_from_config(syscfg, 1, 1);
            printf("  %s\n", uri1);
            free(uri1);

            char *uri2 = hpplite_da_uri_from_config(syscfg, 1, 100);
            printf("  %s\n", uri2);
            free(uri2);

            hpplite_l1_system_config_free(syscfg);
        }
        /* Note: L1 disconnect is handled automatically by close hook */
    } else {
        printf("  (L1 not connected - network may be unavailable)\n");
    }

    sqlite3_close(db);
    printf("\nDone!\n");
    return 0;
}
