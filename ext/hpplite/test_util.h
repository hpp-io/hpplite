/*
** HPPLite Test/Example Utilities
**
** Common functions for tests and examples:
** - .env file loading
** - Key generation/derivation via cast CLI
** - Wallet funding via cast CLI
**
** Requires: cast (from foundry), jq
*/

#ifndef HPPLITE_TEST_UTIL_H
#define HPPLITE_TEST_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

/* Default HPP Sepolia settings */
#define HPPLITE_DEFAULT_RPC "https://sepolia.hpp.io"
#define HPPLITE_DEFAULT_FACTORY "0xa80365fd5b4e7a22df13de334e3ef0c74032d345"

/* Run command and capture first line of output */
static inline char *test_util_run_cmd(const char *cmd) {
    static char buf[512];
    FILE *fp = popen(cmd, "r");
    if (!fp) return NULL;
    if (!fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        return NULL;
    }
    pclose(fp);
    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';
    return buf;
}

/* Load value from .env file */
static inline char *test_util_load_env(const char *path, const char *key) {
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    static char value[512];
    char line[1024];
    size_t keylen = strlen(key);

    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#' || line[0] == '\n') continue;
        if (strncmp(line, key, keylen) == 0 && line[keylen] == '=') {
            char *v = line + keylen + 1;
            char *nl = strchr(v, '\n');
            if (nl) *nl = '\0';
            strncpy(value, v, sizeof(value) - 1);
            fclose(f);
            return value;
        }
    }
    fclose(f);
    return NULL;
}

/* Get private key from env var or .env files */
static inline const char *test_util_get_master_key(void) {
    const char *key = getenv("HPPLITE_PRIVATE_KEY");
    if (key) return key;
    key = test_util_load_env(".env", "HPPLITE_PRIVATE_KEY");
    if (key) return key;
    key = test_util_load_env("../.env", "HPPLITE_PRIVATE_KEY");
    return key;
}

/* Check if cast CLI is available */
static inline int test_util_has_cast(void) {
    return system("command -v cast >/dev/null 2>&1") == 0;
}

/* Parse hex string to bytes */
static inline int test_util_hex_to_bytes(const char *hex, unsigned char *out, int len) {
    if (!hex) return -1;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    if ((int)strlen(hex) < len * 2) return -1;
    for (int i = 0; i < len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        out[i] = (unsigned char)byte;
    }
    return 0;
}

/* Generate new random private key - caller provides buffer */
static inline int test_util_generate_key(char *out, size_t out_size) {
    char *result = test_util_run_cmd("cast wallet new --json 2>/dev/null | jq -r '.[0].private_key'");
    if (!result || strlen(result) < 64) return -1;
    strncpy(out, result, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* Derive address from private key - caller provides buffer */
static inline int test_util_get_address(const char *privkey, char *out, size_t out_size) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "cast wallet address \"%s\" 2>/dev/null", privkey);
    char *result = test_util_run_cmd(cmd);
    if (!result) return -1;
    strncpy(out, result, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

/* Fund address from master key */
static inline int test_util_fund_wallet(const char *to_address, const char *master_key,
                                        const char *rpc_url, const char *amount) {
    char cmd[768];
    /* Sends from the master wallet follow each other quickly (one per test
    ** or example): a nonce race or a slow RPC fails one now and then.
    ** Retry, and show cast's error on the last attempt instead of hiding it. */
    for (int attempt = 1; attempt <= 3; attempt++) {
        snprintf(cmd, sizeof(cmd),
            "cast send \"%s\" --value %s --private-key \"%s\" --rpc-url \"%s\" >/dev/null %s",
            to_address, amount, master_key, rpc_url, attempt < 3 ? "2>&1" : "");
        if (system(cmd) == 0) return 0;
        sleep(3 * attempt);
    }
    return -1;
}

/* All-in-one: generate key, derive address, fund from master
 * privkey_out and address_out are caller-provided buffers */
static inline int test_util_create_funded_wallet(char *privkey_out, size_t privkey_size,
                                                  char *address_out, size_t address_size,
                                                  const char *rpc_url, const char *amount) {
    const char *master = test_util_get_master_key();
    if (!master) {
        fprintf(stderr, "ERROR: No master key found (set HPPLITE_PRIVATE_KEY or .env)\n");
        return -1;
    }

    if (test_util_generate_key(privkey_out, privkey_size) != 0) {
        fprintf(stderr, "ERROR: Failed to generate key (is cast installed?)\n");
        return -1;
    }

    if (test_util_get_address(privkey_out, address_out, address_size) != 0) {
        fprintf(stderr, "ERROR: Failed to derive address\n");
        return -1;
    }

    if (test_util_fund_wallet(address_out, master, rpc_url, amount) != 0) {
        fprintf(stderr, "ERROR: Failed to fund wallet\n");
        return -1;
    }

    return 0;
}

#endif /* HPPLITE_TEST_UTIL_H */
