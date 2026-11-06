/*
** HPPLite DA URI Implementation
*/

#include "da_uri.h"
#include "eth/eth_client.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/*
** Built-in network aliases
** Factory addresses are per-network constants
*/
const HppliteNetworkAlias HPPLITE_NETWORKS[] = {
    /* Measured on HPP Sepolia 2026-10-30, below what the contract allows
    ** (128 KB batches): a transaction may use at most 32M gas (its block gas
    ** limit is 2^50; the transaction cap is what binds - batches in contract
    ** storage cost ~700 gas/byte, so ~44 KB is the most one can carry), and
    ** its RPC answers "oversized data" above 84864 bytes of calldata. Ethereum:
    ** EIP-7825 caps a transaction at 2^24 gas; geth's txpool takes 128 KB. */
    /* HPP anchors to L1 every 30 minutes: a batch posted more often only waits
    ** in its queue, and each costs a commitment (#162), so that is the interval
    ** on HPP unless interval= says otherwise */
    /* the canonical operators (hpp-io/hpplite-operators): HPP's store and relay on the network, store=on / relay=on */
    {"hpp-sepolia", 181228, "https://sepolia.hpp.io", "0xbce61e56f84620bc4365e95126f82b51faed7b4e", 40000, 32000000, 30 * 60 * 1000,
     "https://store.sepolia.hpplite.hpp.io", "https://relay.sepolia.hpplite.hpp.io"},
    {"hpp-mainnet", 181227, "https://mainnet.hpp.io", NULL, 40000, 32000000, 30 * 60 * 1000, NULL, NULL},  /* Not deployed yet */
    {"ethereum",    1,      "https://eth.llamarpc.com", NULL, 20000, 16777216, 0, NULL, NULL},
    {"sepolia",     11155111, "https://sepolia.drpc.org", NULL, 20000, 16777216, 0, NULL, NULL},
    {NULL, 0, NULL, NULL, 0, 0, 0, NULL, NULL}
};

int hpplite_network_batch_interval_ms(uint64_t chainId) {
    int i;
    for (i = 0; HPPLITE_NETWORKS[i].alias; i++) {
        if (HPPLITE_NETWORKS[i].chainId == chainId) return HPPLITE_NETWORKS[i].batchIntervalMs;
    }
    return 0;
}

uint64_t hpplite_network_max_tx_gas(uint64_t chainId) {
    int i;
    for (i = 0; HPPLITE_NETWORKS[i].alias; i++) {
        if (HPPLITE_NETWORKS[i].chainId == chainId) return HPPLITE_NETWORKS[i].maxTxGas;
    }
    return 0;
}

size_t hpplite_network_max_tx_bytes(uint64_t chainId) {
    int i;
    for (i = 0; HPPLITE_NETWORKS[i].alias; i++) {
        if (HPPLITE_NETWORKS[i].chainId == chainId) return HPPLITE_NETWORKS[i].maxTxBytes;
    }
    return 0;
}

const int HPPLITE_NETWORKS_COUNT = 4;

const char *hpplite_network_store_url(uint64_t chainId) {
    int i;
    for (i = 0; HPPLITE_NETWORKS[i].alias; i++) if (HPPLITE_NETWORKS[i].chainId == chainId) return HPPLITE_NETWORKS[i].storeUrl;
    return NULL;
}

const char *hpplite_network_relay_url(uint64_t chainId) {
    int i;
    for (i = 0; HPPLITE_NETWORKS[i].alias; i++) if (HPPLITE_NETWORKS[i].chainId == chainId) return HPPLITE_NETWORKS[i].relayUrl;
    return NULL;
}

const char *hpplite_da_scheme_str(HppliteDAScheme scheme) {
    switch (scheme) {
        case HPPLITE_DA_HPPDA: return "hppda";
        case HPPLITE_DA_IPFS:  return "ipfs";
        case HPPLITE_DA_FILE:  return "file";
        case HPPLITE_DA_HTTP:  return "http";
        default: return "unknown";
    }
}

HppliteDAScheme hpplite_da_scheme_parse(const char *str) {
    if (!str) return HPPLITE_DA_UNKNOWN;
    if (strcmp(str, "hppda") == 0) return HPPLITE_DA_HPPDA;
    if (strcmp(str, "ipfs") == 0) return HPPLITE_DA_IPFS;
    if (strcmp(str, "file") == 0) return HPPLITE_DA_FILE;
    if (strcmp(str, "http") == 0 || strcmp(str, "https") == 0) return HPPLITE_DA_HTTP;
    return HPPLITE_DA_UNKNOWN;
}

uint64_t hpplite_da_resolve_alias(const char *alias) {
    if (!alias) return 0;

    /* Check if it's already a number */
    int isNum = 1;
    for (const char *p = alias; *p; p++) {
        if (!isdigit(*p)) {
            isNum = 0;
            break;
        }
    }
    if (isNum) {
        return strtoull(alias, NULL, 10);
    }

    /* Look up alias */
    for (int i = 0; HPPLITE_NETWORKS[i].alias; i++) {
        if (strcmp(alias, HPPLITE_NETWORKS[i].alias) == 0) {
            return HPPLITE_NETWORKS[i].chainId;
        }
    }

    return 0;
}

const char *hpplite_da_get_rpc_url(uint64_t chainId) {
    for (int i = 0; HPPLITE_NETWORKS[i].alias; i++) {
        if (HPPLITE_NETWORKS[i].chainId == chainId) {
            return HPPLITE_NETWORKS[i].rpcUrl;
        }
    }
    return NULL;
}

const char *hpplite_da_get_factory(uint64_t chainId) {
    for (int i = 0; HPPLITE_NETWORKS[i].alias; i++) {
        if (HPPLITE_NETWORKS[i].chainId == chainId) {
            return HPPLITE_NETWORKS[i].factory;
        }
    }
    return NULL;
}

const HppliteNetworkAlias *hpplite_da_lookup_network(const char *alias) {
    if (!alias) return NULL;
    for (int i = 0; HPPLITE_NETWORKS[i].alias; i++) {
        if (strcmp(alias, HPPLITE_NETWORKS[i].alias) == 0) {
            return &HPPLITE_NETWORKS[i];
        }
    }
    return NULL;
}

HppliteDAUri *hpplite_da_uri_parse(const char *uri) {
    if (!uri) return NULL;

    HppliteDAUri *parsed = calloc(1, sizeof(HppliteDAUri));
    if (!parsed) return NULL;

    parsed->raw = strdup(uri);

    /* Find scheme separator :// */
    const char *schemeEnd = strstr(uri, "://");
    if (!schemeEnd) {
        hpplite_da_uri_free(parsed);
        return NULL;
    }

    /* Extract scheme */
    size_t schemeLen = schemeEnd - uri;
    char *scheme = malloc(schemeLen + 1);
    memcpy(scheme, uri, schemeLen);
    scheme[schemeLen] = '\0';

    parsed->scheme = hpplite_da_scheme_parse(scheme);
    free(scheme);

    const char *rest = schemeEnd + 3; /* Skip :// */

    switch (parsed->scheme) {
        case HPPLITE_DA_HPPDA: {
            /* Parse: <chainId>/<contract>/<height> or <chainId>/<contract>/<from>-<to> */
            /* Or: <alias>/<contract>/<height> */

            /* Find first slash (after chainId/alias) */
            const char *slash1 = strchr(rest, '/');
            if (!slash1) {
                hpplite_da_uri_free(parsed);
                return NULL;
            }

            /* Extract chain ID or alias */
            size_t chainLen = slash1 - rest;
            char *chainStr = malloc(chainLen + 1);
            memcpy(chainStr, rest, chainLen);
            chainStr[chainLen] = '\0';

            parsed->chainId = hpplite_da_resolve_alias(chainStr);
            free(chainStr);

            if (parsed->chainId == 0) {
                hpplite_da_uri_free(parsed);
                return NULL;
            }

            /* Find second slash (after contract) */
            const char *slash2 = strchr(slash1 + 1, '/');
            if (!slash2) {
                hpplite_da_uri_free(parsed);
                return NULL;
            }

            /* Extract contract address */
            size_t contractLen = slash2 - (slash1 + 1);
            char *contractStr = malloc(contractLen + 1);
            memcpy(contractStr, slash1 + 1, contractLen);
            contractStr[contractLen] = '\0';

            if (eth_hex_decode(contractStr, parsed->contract, 20) != 0) {
                free(contractStr);
                hpplite_da_uri_free(parsed);
                return NULL;
            }
            free(contractStr);

            /* Parse height(s) */
            const char *heightStr = slash2 + 1;
            const char *dash = strchr(heightStr, '-');
            if (dash) {
                /* Range: from-to */
                parsed->heightFrom = strtoull(heightStr, NULL, 10);
                parsed->heightTo = strtoull(dash + 1, NULL, 10);
            } else {
                /* Single height */
                parsed->heightFrom = strtoull(heightStr, NULL, 10);
                parsed->heightTo = parsed->heightFrom;
            }
            break;
        }

        case HPPLITE_DA_IPFS:
        case HPPLITE_DA_FILE:
        case HPPLITE_DA_HTTP:
            /* Just store the path */
            parsed->path = strdup(rest);
            break;

        default:
            hpplite_da_uri_free(parsed);
            return NULL;
    }

    return parsed;
}

void hpplite_da_uri_free(HppliteDAUri *uri) {
    if (!uri) return;
    if (uri->raw) free(uri->raw);
    if (uri->path) free(uri->path);
    free(uri);
}

char *hpplite_da_uri_build(
    HppliteDAScheme scheme,
    uint64_t chainId,
    const unsigned char *contract,
    uint64_t heightFrom,
    uint64_t heightTo
) {
    char *uri = malloc(256);
    if (!uri) return NULL;

    switch (scheme) {
        case HPPLITE_DA_HPPDA: {
            char contractHex[43]; /* 0x + 40 hex + null */
            contractHex[0] = '0';
            contractHex[1] = 'x';
            for (int i = 0; i < 20; i++) {
                sprintf(contractHex + 2 + i * 2, "%02x", contract[i]);
            }

            if (heightFrom == heightTo) {
                snprintf(uri, 256, "hppda://%llu/%s/%llu",
                         (unsigned long long)chainId,
                         contractHex,
                         (unsigned long long)heightFrom);
            } else {
                snprintf(uri, 256, "hppda://%llu/%s/%llu-%llu",
                         (unsigned long long)chainId,
                         contractHex,
                         (unsigned long long)heightFrom,
                         (unsigned long long)heightTo);
            }
            break;
        }

        default:
            free(uri);
            return NULL;
    }

    return uri;
}

char *hpplite_da_uri_from_config(
    const HppliteSystemConfig *config,
    uint64_t heightFrom,
    uint64_t heightTo
) {
    if (!config) return NULL;

    return hpplite_da_uri_build(
        HPPLITE_DA_HPPDA,
        config->chainId,
        config->daContract,
        heightFrom,
        heightTo
    );
}
