/*
** HPPLite Configuration Implementation
**
** Layered config: Defaults → Env vars → URI params
*/

#include "config.h"
#include "hpplite.h"
#include "spend.h"
#include "keystore.h"
#include <sys/stat.h>
#include <errno.h>
#include "da_uri.h"
#include "hpplite_sqlite.h"
#include "crypto.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

/*
** Helper: duplicate string safely
*/
static char *safe_strdup(const char *s) {
    return s ? strdup(s) : NULL;
}

/*
** Helper: parse hex string to bytes
*/
static int hex_to_bytes(const char *hex, unsigned char *out, int len) {
    if (!hex) return -1;

    /* Skip 0x prefix */
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex += 2;
    }

    size_t hexLen = strlen(hex);
    if (hexLen != (size_t)(len * 2)) return -1;

    for (int i = 0; i < len; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        out[i] = (unsigned char)byte;
    }

    return 0;
}

/*
** Helper: parse duration string (e.g., "5s", "100ms", "1m")
*/
/* on/off, true/false, yes/no, 1/0 */
static int parse_bool(const char *z) {
    if (!z) return 1;
    if (!sqlite3_stricmp(z, "off") || !sqlite3_stricmp(z, "false") || !sqlite3_stricmp(z, "no") || !strcmp(z, "0")) return 0;
    return 1;
}

/* on/1/true/yes -> 1; anything else -> 0: a switch given in a URI or the environment (#265) */
static int parse_on(const char *v) {
    return v && (strcasecmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcasecmp(v, "true") == 0 || strcasecmp(v, "yes") == 0);
}
/* off/0/false/no -> 1: a switch that is on unless said otherwise (#266) */
static int parse_off(const char *v) {
    return v && (strcasecmp(v, "off") == 0 || strcmp(v, "0") == 0 || strcasecmp(v, "false") == 0 || strcasecmp(v, "no") == 0);
}

static int parse_duration_ms(const char *str) {
    if (!str) return -1;

    int value = atoi(str);
    if (value == 0 && (str[0] == '0')) return 0;   /* "0", "0s", "0ms": no timer */
    if (value <= 0) return -1;

    /* Find unit suffix */
    const char *p = str;
    while (*p && (isdigit(*p) || *p == '.')) p++;

    if (*p == 's' || *p == 'S') {
        return value * 1000;
    } else if (*p == 'm' && (*(p+1) == 's' || *(p+1) == 'S')) {
        return value;
    } else if (*p == 'm' || *p == 'M') {
        return value * 60 * 1000;
    } else if (*p == 'h' || *p == 'H') {
        return value * 60 * 60 * 1000;
    } else if (*p == '\0') {
        /* No unit = seconds */
        return value * 1000;
    }

    return -1;
}

/* keep= (#223): "all" (0: every batch kept), or N with s, m, h or d (no unit: days) - in seconds; -1 malformed */
static int64_t parse_keep_seconds(const char *str) {
    char *end = NULL;
    long long v;
    if (!str) return -1;
    if (strcmp(str, "all") == 0) return 0;
    v = strtoll(str, &end, 10);
    if (v <= 0 || end == str) return -1;
    if (*end == 's' || *end == 'S') return v;
    if (*end == 'm' || *end == 'M') return v * 60;
    if (*end == 'h' || *end == 'H') return v * 3600;
    if (*end == 'd' || *end == 'D' || *end == 0) return v * 86400;
    return -1;
}


static char gConfigError[160];
const char *hpplite_config_error(void) { return gConfigError[0] ? gConfigError : NULL; }

/* capture= and determinism= are gone (v0.8, #253): HPPLite captures pages, which replicate any SQL. A setting from
** before refuses the open rather than being read as a default in its place (#80) */
static void refuse_capture(const char *val) {
    (void)val;
    snprintf(gConfigError, sizeof(gConfigError), "capture= is gone (v0.8, #253): HPPLite captures pages; row and statement capture were removed (v0.7 is the last release with them)");
}
static void refuse_determinism(const char *val) {
    (void)val;
    snprintf(gConfigError, sizeof(gConfigError), "determinism= is gone (v0.8, #253): page capture replicates any SQL, nothing is refused for replay's sake");
}

/* The money knobs are the deployment's, not the connection string's (v0.8, #254): a budget belongs with the wallet's
** key in the environment, not in every URI that opens the file. Each in a URI refuses the open and names its variable */
static const struct { const char *uri, *env; } gMoneyKnobs[] = {
    { HPPLITE_URI_STORE_CAP, HPPLITE_ENV_STORE_CAP },     { HPPLITE_URI_STORE_PERMIT, HPPLITE_ENV_STORE_PERMIT },
    { HPPLITE_URI_SPEND_CAP, HPPLITE_ENV_SPEND_CAP },     { HPPLITE_URI_LOW_BALANCE, HPPLITE_ENV_LOW_BALANCE },
    { HPPLITE_URI_RELAY_CAP, HPPLITE_ENV_RELAY_CAP },     { HPPLITE_URI_STORE_FALLBACK, HPPLITE_ENV_STORE_FALLBACK },
};
#define N_MONEY_KNOBS (int)(sizeof(gMoneyKnobs) / sizeof(gMoneyKnobs[0]))
static void refuse_money_knob(int i) {
    snprintf(gConfigError, sizeof(gConfigError), "%s= is not a URI setting (v0.8, #254): a budget is the deployment's; set %s in the environment",
             gMoneyKnobs[i].uri, gMoneyKnobs[i].env);
}

/* from= and soft= are gone (v0.8, #255): where a rebuild starts and whether the soft tier is followed are what the
** node is, not settings - hpplite-verify decided both for the user and overrode what was passed */
static void refuse_from(void) {
    snprintf(gConfigError, sizeof(gConfigError), "from= is gone (v0.8, #255): a replica starts from the latest snapshot the anchors name; a verifier (role=verify) from batch 1, and from the oldest snapshot kept when the store has pruned it");
}
static void refuse_soft(void) {
    snprintf(gConfigError, sizeof(gConfigError), "soft= is gone (v0.8, #255): a verifier follows the store's batches past the anchor (hpplite-verify reports them as the soft tier); with syncto= or syncat= it stays at the hard tier");
}

/* role=verify, nothing else (#248): sequencer and observer were parsed and read nowhere - a sequencer needs no role= */
/* fleet= (#316): a database in a shared rollup is named by owner= and dbname= alone; admit= carries the fleet's Admit */
static void fleet_param(HppliteConfig *cfg, const char *name, const char *val) {
    if (!val) return;
    if (strcmp(name, "fleet") == 0) { if (hex_to_bytes(val, cfg->fleet, 20) == 0) cfg->hasFleet = 1;
                                      else snprintf(gConfigError, sizeof(gConfigError), "fleet=%.48s: not an address", val); }
    else if (strcmp(name, "owner") == 0) { if (hex_to_bytes(val, cfg->fleetOwner, 20) == 0) cfg->hasFleetOwner = 1;
                                           else snprintf(gConfigError, sizeof(gConfigError), "owner=%.48s: not an address", val); }
    else if (strcmp(name, "admit") == 0) {
        const char *colon = strchr(val, ':');
        cfg->admitIssuedAt = strtoull(val, NULL, 10);
        if (!colon || !cfg->admitIssuedAt || hex_to_bytes(colon + 1, cfg->admitSig, 65) != 0)
            snprintf(gConfigError, sizeof(gConfigError), "admit=: <issuedAt>:0x<65-byte signature>, the fleet's Admit");
        else cfg->hasAdmit = 1;
    }
}

/* with fleet=: owner=, dbname=, store= and relay= required; what names a rollup or is a fleet anchor's job refuses the
** open from any source - the URI or the environment (a sidecar's environment is where an old HPPLITE_ANCHOR stays) - and
** the dbId. uri[]: contract, factory, anchor, checkpoint, keep, snapshot, anchorclose given in the URI */
static void fleet_check(HppliteConfig *cfg, const int uri[7]) {
    static const struct { const char *name, *env; int anchors; } k[7] = {
        { "contract", HPPLITE_ENV_CONTRACT, 0 }, { "factory", HPPLITE_ENV_FACTORY, 0 }, { "anchor", "HPPLITE_ANCHOR", 1 },
        { "checkpoint", HPPLITE_ENV_CHECKPOINT, 1 }, { "keep", "HPPLITE_KEEP", 1 }, { "snapshot", "HPPLITE_SNAPSHOT", 1 },
        { "anchorclose", "HPPLITE_ANCHOR_CLOSE", 1 } };
    unsigned char word[64];
    int i;
    if (!cfg->hasFleet || gConfigError[0]) return;
    if (!cfg->hasFleetOwner) { snprintf(gConfigError, sizeof(gConfigError), "fleet=: owner= is required (with dbname= it names the database)"); return; }
    if (!cfg->dbName || !cfg->dbName[0]) { snprintf(gConfigError, sizeof(gConfigError), "fleet=: dbname= is required (with owner= it names the database)"); return; }
    if (!cfg->storeUrl) { snprintf(gConfigError, sizeof(gConfigError), "fleet=: store= is required: a fleet's batches live at a store"); return; }
    if (!cfg->relayUrl) { snprintf(gConfigError, sizeof(gConfigError), "fleet=: relay= is required: a fleet database joins and claims through a relay"); return; }
    for (i = 0; i < 7; i++) {
        const char *ev = getenv(k[i].env);
        int env = ev != NULL && ev[0];
        if (!uri[i] && !env) continue;
        snprintf(gConfigError, sizeof(gConfigError), "%s%s%s with fleet=: %s%s", uri[i] ? k[i].name : k[i].env, uri[i] ? "=" : " (environment)",
                 "", k[i].anchors ? "the fleet's anchor anchors its databases" : "a database in a fleet is named by fleet=, owner= and dbname= alone",
                 (i == 4 || i == 5) ? " (snapshots and pruning in a fleet: #314)" : "");
        return;
    }
    memset(word, 0, 12);
    memcpy(word + 12, cfg->fleetOwner, 20);
    hpplite_keccak256((const unsigned char *)cfg->dbName, strlen(cfg->dbName), word + 32);
    hpplite_keccak256(word, 64, cfg->dbId);
}

static void parse_role(HppliteConfig *cfg, const char *val, int source) {
    if (strcasecmp(val, "verify") == 0) { cfg->roleVerify = 1; cfg->roleSource = source; return; }
    snprintf(gConfigError, sizeof(gConfigError), "role=%.40s: role= takes verify only (an audit connection); a sequencer needs no role=", val);
}

HppliteConfig *hpplite_config_create(void) {
    HppliteConfig *cfg = calloc(1, sizeof(HppliteConfig));
    if (!cfg) return NULL;
    gConfigError[0] = 0;

    /* Set defaults */
    cfg->chainId = HPPLITE_DEFAULT_CHAIN_ID;
    cfg->rpcUrl = strdup(HPPLITE_DEFAULT_RPC_URL);
    cfg->dataDir = strdup(HPPLITE_DEFAULT_DATA_DIR);
    cfg->batchIntervalMs = 5000;
   /* 5 seconds default */
    cfg->anchorIntervalMs = 30 * 60 * 1000;   /* HPP's cadence (#164); anchor= overrides */
    cfg->anchorClose = 1;                      /* the close anchors; anchorclose=off leaves it to the timer (#266) */
    cfg->encrypt = 1;             /* everything posted is public otherwise */
    cfg->compress = 1;            /* smaller batches everywhere they go (#12) */
    cfg->storeFallback = 1;       /* a store that does not answer never holds a batch back (#11) */

    return cfg;
}

void hpplite_config_free(HppliteConfig *cfg) {
    if (!cfg) return;
    memset(cfg->privkey, 0, sizeof(cfg->privkey));   /* key material does not outlive the config */
    if (cfg->nodeId) free(cfg->nodeId);
    if (cfg->rpcUrl) free(cfg->rpcUrl);
    if (cfg->dataDir) free(cfg->dataDir);
    if (cfg->dbPath) free(cfg->dbPath);
    free(cfg->storeUrl);
    free(cfg->relayUrl);
    free(cfg->dbName);
    free(cfg);
}

int hpplite_config_load_env(HppliteConfig *cfg) {
    if (!cfg) return -1;

    const char *val;

    /* Role */
    val = getenv(HPPLITE_ENV_ROLE);
    if (val) parse_role(cfg, val, 1);

    /* Chain ID */
    val = getenv(HPPLITE_ENV_CHAIN_ID);
    if (val) {
        cfg->chainId = strtoull(val, NULL, 10);
        cfg->chainIdSource = 1;
    }

    /* RPC URL */
    val = getenv(HPPLITE_ENV_RPC_URL);
    if (val) {
        free(cfg->rpcUrl);
        cfg->rpcUrl = strdup(val);
        cfg->rpcUrlSource = 1;
    }

    /* Contract */
    val = getenv(HPPLITE_ENV_CONTRACT);
    if (val) {
        if (hpplite_config_parse_contract(cfg, val) == 0) {
            cfg->contractSource = 1;
        }
    }

    /* Factory */
    val = getenv(HPPLITE_ENV_FACTORY);
    if (val) {
        if (hex_to_bytes(val, cfg->factory, 20) == 0) {
            cfg->hasFactory = 1;
        }
    }

    /* Private key */
    val = getenv(HPPLITE_ENV_PRIVKEY);
    if (val) {
        hpplite_config_parse_privkey(cfg, val);
    }

    /* Private key file */
    val = getenv(HPPLITE_ENV_PRIVKEY_FILE);
    if (val) {
        hpplite_config_load_privkey_file(cfg, val);
    }

    /* Keystore V3 (#17) */
    val = getenv(HPPLITE_ENV_KEYSTORE);
    if (val) {
        hpplite_config_load_keystore(cfg, val, NULL);
    }

    /* Viewing key (#15) */
    val = getenv(HPPLITE_ENV_VIEWKEY);
    if (val) {
        hpplite_config_parse_viewkey(cfg, val);
    }
    val = getenv(HPPLITE_ENV_OWNERKEY);
    if (val && val[0] && hpplite_config_parse_ownerkey(cfg, val) != 0) fprintf(stderr, "hpplite: %s: not a compressed public key (0x02/0x03 + 64 hex); ignored\n", HPPLITE_ENV_OWNERKEY);

    /* Data dir */
    val = getenv(HPPLITE_ENV_DATA_DIR);
    if (val) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(val);
    }


    /* Batch interval */
    val = getenv(HPPLITE_ENV_BATCH_INTERVAL);
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) { cfg->batchIntervalMs = ms; cfg->intervalGiven = 1; }
    }
    val = getenv("HPPLITE_ANCHOR");
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) { cfg->anchorIntervalMs = ms; cfg->anchorGiven = 1; }
    }
    val = getenv("HPPLITE_KEEP");
    if (val) { int64_t k = parse_keep_seconds(val); if (k >= 0) cfg->keepSeconds = k; }
    val = getenv("HPPLITE_KEEP_LEASE");
    if (val) cfg->keepLease = parse_on(val);
    val = getenv("HPPLITE_ANCHOR_CLOSE");
    if (val) cfg->anchorClose = !parse_off(val);
    val = getenv(HPPLITE_ENV_FROM);
    if (val) refuse_from();
    val = getenv("HPPLITE_SNAPSHOT");
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) cfg->snapshotIntervalMs = ms;
    }

    /* Encryption */
    val = getenv(HPPLITE_ENV_ENCRYPT);
    if (val) cfg->encrypt = parse_bool(val);
    val = getenv(HPPLITE_ENV_COMPRESS);
    if (val) cfg->compress = parse_bool(val);

    val = getenv(HPPLITE_ENV_DETERMINISM);
    if (val) refuse_determinism(val);
    val = getenv(HPPLITE_ENV_CAPTURE);
    if (val) refuse_capture(val);
    val = getenv(HPPLITE_ENV_DBNAME);
    if (val && val[0]) { free(cfg->dbName); cfg->dbName = strdup(val); }
    val = getenv(HPPLITE_ENV_STORE);
    if (val && val[0]) { free(cfg->storeUrl); cfg->storeUrl = strdup(val); }
    val = getenv(HPPLITE_ENV_STORE_CAP);
    if (val && atoll(val) > 0) cfg->storeCap = (uint64_t)atoll(val);
    val = getenv(HPPLITE_ENV_STORE_PERMIT);
    if (val && val[0] >= '0' && val[0] <= '9') { cfg->storePermit = (uint64_t)atoll(val); cfg->storePermitSet = 1; }
    val = getenv(HPPLITE_ENV_FUNDER);
    if (val && hex_to_bytes(val, cfg->funder, 20) == 0) cfg->hasFunder = 1;
    val = getenv(HPPLITE_ENV_SOFT);
    if (val) refuse_soft();
    val = getenv(HPPLITE_ENV_SPEND_CAP);
    if (val && val[0] && hpplite_spend_parse_cap(val, &cfg->spendCap, &cfg->spendWindow) != 0) fprintf(stderr, "hpplite: %s=%s: not N, N/hour or N/day; ignored\n", HPPLITE_ENV_SPEND_CAP, val);
    val = getenv(HPPLITE_ENV_LOW_BALANCE);
    if (val && val[0]) cfg->lowBalanceHours = atoi(val) > 0 ? atoi(val) : -1;
    val = getenv(HPPLITE_ENV_RELAY);
    if (val && val[0]) { free(cfg->relayUrl); cfg->relayUrl = strdup(val); }
    val = getenv(HPPLITE_ENV_RELAY_CAP);
    if (val && atoll(val) > 0) cfg->relayCap = (uint64_t)atoll(val);
    val = getenv(HPPLITE_ENV_STORE_FALLBACK);
    if (val) cfg->storeFallback = strcasecmp(val, "off") != 0 && strcmp(val, "0") != 0;
    val = getenv(HPPLITE_ENV_MAX_TX);
    if (val && atol(val) > 0) cfg->maxTxBytes = (size_t)atol(val);
    val = getenv(HPPLITE_ENV_MAX_TX_GAS);
    if (val && atoll(val) > 0) cfg->maxTxGas = (uint64_t)atoll(val);

    /* Checkpoint interval */
    val = getenv(HPPLITE_ENV_CHECKPOINT);
    if (val && atoi(val) > 0) cfg->checkpointInterval = atoi(val);

    /* the chain's cadence applies to whatever goes to the chain (#162, #164): the batches
    ** of a node without a store, the anchors of a node with one */
    if (cfg->aliasIntervalMs && cfg->storeUrl) { if (!cfg->anchorGiven) cfg->anchorIntervalMs = cfg->aliasIntervalMs; }
    else if (cfg->aliasIntervalMs && !cfg->intervalGiven) cfg->batchIntervalMs = cfg->aliasIntervalMs;
    return 0;
}

/*
** Simple URI parameter extraction
** Parses: file:path?key1=val1&key2=val2
*/
static const char *get_uri_param(const char *uri, const char *key, char *buf, size_t buflen) {
    if (!uri || !key) return NULL;

    /* Find ? */
    const char *query = strchr(uri, '?');
    if (!query) return NULL;
    query++;

    size_t keyLen = strlen(key);
    const char *p = query;

    while (*p) {
        /* Check if this is our key */
        if (strncmp(p, key, keyLen) == 0 && p[keyLen] == '=') {
            /* %HH decoded, as sqlite3_uri_parameter() does (#36); a value
            ** longer than the buffer is refused, not cut (a key file path
            ** or an RPC URL cut short would be another one) */
            const char *v = p + keyLen + 1;
            size_t n = 0;
            while (*v && *v != '&' && *v != '#') {
                int c = (unsigned char)*v++;
                if (c == '%' && isxdigit((unsigned char)v[0]) && isxdigit((unsigned char)v[1])) {
                    char hex[3] = { v[0], v[1], 0 };
                    c = (int)strtol(hex, NULL, 16);
                    v += 2;
                }
                if (n + 1 >= buflen) {
                    fprintf(stderr, "hpplite: URI parameter %s is longer than %zu bytes; ignored\n", key, buflen - 1);
                    return NULL;
                }
                if (c == 0) {
                    /* a %00 would end the value there, as a path or name: not one it reads as (#78) */
                    fprintf(stderr, "hpplite: URI parameter %s contains %%00; ignored\n", key);
                    return NULL;
                }
                buf[n++] = (char)c;
            }
            buf[n] = '\0';
            return buf;
        }

        /* Move to next param */
        while (*p && *p != '&') p++;
        if (*p == '&') p++;
    }

    return NULL;
}

/*
** Extract database path from URI
** file:path?params → path
** file://path?params → path
*/
static char *extract_db_path(const char *uri) {
    if (!uri) return NULL;

    const char *path = uri;

    /* Skip file: prefix */
    if (strncmp(uri, "file:", 5) == 0) {
        path = uri + 5;
        /* Skip optional // */
        if (path[0] == '/' && path[1] == '/') {
            path += 2;
        }
    }

    /* Find end (? or end of string) */
    const char *end = strchr(path, '?');
    if (!end) end = path + strlen(path);

    size_t len = end - path;
    char *result = malloc(len + 1);
    memcpy(result, path, len);
    result[len] = '\0';

    return result;
}

int hpplite_config_load_uri(HppliteConfig *cfg, const char *uri) {
    if (!cfg || !uri) return -1;

    char buf[4096];

    /* Extract database path */
    cfg->dbPath = extract_db_path(uri);

    /* Check if hpplite is enabled */
    if (!get_uri_param(uri, HPPLITE_URI_ENABLED, buf, sizeof(buf))) {
        /* hpplite param not present - not an HPPLite database */
        return 1;
    }

    /* Role */
    if (get_uri_param(uri, HPPLITE_URI_ROLE, buf, sizeof(buf))) parse_role(cfg, buf, 2);

    /* Chain ID (numeric) */
    if (get_uri_param(uri, HPPLITE_URI_CHAIN_ID, buf, sizeof(buf))) {
        cfg->chainId = strtoull(buf, NULL, 10);
        cfg->chainIdSource = 2;
    }

    /* L1 network alias; "local" is no chain at all, batches finalized here (#38) */
    if (get_uri_param(uri, HPPLITE_URI_L1, buf, sizeof(buf))) {
        const HppliteNetworkAlias *network = strcasecmp(buf, "local") == 0 ? NULL : hpplite_da_lookup_network(buf);
        if (strcasecmp(buf, "local") == 0) cfg->localOnly = 1;
        if (strncmp(buf, "file://", 7) == 0) {
            /* a directory is the chain and the rollup (#35): no contract, no factory, no wallet needed */
            free(cfg->rpcUrl);
            cfg->rpcUrl = strdup(buf);
            cfg->rpcUrlSource = 2;
            memset(cfg->contract, 0, 20);
            cfg->hasContract = 1;
            cfg->contractSource = 2;
        }
        if (network) {
            cfg->chainId = network->chainId;
            cfg->chainIdSource = 2;
            cfg->aliasIntervalMs = network->batchIntervalMs;

            /* Also set RPC URL from alias */
            if (network->rpcUrl) {
                free(cfg->rpcUrl);
                cfg->rpcUrl = strdup(network->rpcUrl);
                cfg->rpcUrlSource = 2;
            }

            /* Also set factory from alias (if not already set) */
            if (network->factory && !cfg->hasFactory) {
                if (hex_to_bytes(network->factory, cfg->factory, 20) == 0) {
                    cfg->hasFactory = 1;
                }
            }
        }
    }

    /* RPC URL */
    if (get_uri_param(uri, HPPLITE_URI_RPC_URL, buf, sizeof(buf))) {
        free(cfg->rpcUrl);
        cfg->rpcUrl = strdup(buf);
        cfg->rpcUrlSource = 2;
    }

    /* Contract */
    if (get_uri_param(uri, HPPLITE_URI_CONTRACT, buf, sizeof(buf))) {
        if (hpplite_config_parse_contract(cfg, buf) == 0) {
            cfg->contractSource = 2;
        }
    }

    /* Factory */
    if (get_uri_param(uri, HPPLITE_URI_FACTORY, buf, sizeof(buf))) {
        if (hex_to_bytes(buf, cfg->factory, 20) == 0) {
            cfg->hasFactory = 1;
        }
    }

    /* Private key */
    if (get_uri_param(uri, HPPLITE_URI_PRIVKEY, buf, sizeof(buf))) {
        hpplite_config_parse_privkey(cfg, buf);
    }

    /* Private key file */
    if (get_uri_param(uri, HPPLITE_URI_PRIVKEY_FILE, buf, sizeof(buf))) {
        hpplite_config_load_privkey_file(cfg, buf);
    }

    /* Keystore V3 (#17) */
    if (get_uri_param(uri, HPPLITE_URI_KEYSTORE, buf, sizeof(buf))) {
        hpplite_config_load_keystore(cfg, buf, NULL);
    }

    /* Viewing key (#15) */
    if (get_uri_param(uri, HPPLITE_URI_VIEWKEY, buf, sizeof(buf))) {
        hpplite_config_parse_viewkey(cfg, buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_OWNERKEY, buf, sizeof(buf)) && buf[0] && hpplite_config_parse_ownerkey(cfg, buf) != 0)
        fprintf(stderr, "hpplite: ownerkey=: not a compressed public key (0x02/0x03 + 64 hex); ignored\n");

    /* Data dir */
    if (get_uri_param(uri, HPPLITE_URI_DATA_DIR, buf, sizeof(buf))) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(buf);
    }


    /* Batch interval */
    if (get_uri_param(uri, "interval", buf, sizeof(buf))) {
        int ms = parse_duration_ms(buf);
        if (ms >= 0) { cfg->batchIntervalMs = ms; cfg->intervalGiven = 1; }
    }
    if (get_uri_param(uri, "anchor", buf, sizeof(buf))) {
        int ms = parse_duration_ms(buf);
        if (ms >= 0) { cfg->anchorIntervalMs = ms; cfg->anchorGiven = 1; }
    }
    if (get_uri_param(uri, "keeplease", buf, sizeof(buf))) cfg->keepLease = parse_on(buf);
    if (get_uri_param(uri, "anchorclose", buf, sizeof(buf))) cfg->anchorClose = !parse_off(buf);
    if (get_uri_param(uri, HPPLITE_URI_SNAPSHOT, buf, sizeof(buf))) {
        int ms = parse_duration_ms(buf);
        if (ms >= 0) cfg->snapshotIntervalMs = ms;
    }

    /* Encryption */
    if (get_uri_param(uri, HPPLITE_URI_COMPRESS, buf, sizeof(buf))) {
        cfg->compress = parse_bool(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_ENCRYPT, buf, sizeof(buf))) {
        cfg->encrypt = parse_bool(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_DETERMINISM, buf, sizeof(buf))) refuse_determinism(buf);
    if (get_uri_param(uri, HPPLITE_URI_CAPTURE, buf, sizeof(buf))) refuse_capture(buf);
    if (get_uri_param(uri, HPPLITE_URI_DBNAME, buf, sizeof(buf)) && buf[0]) {
        free(cfg->dbName);
        cfg->dbName = strdup(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_MAX_TX, buf, sizeof(buf)) && atol(buf) > 0) {
        cfg->maxTxBytes = (size_t)atol(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_STORE, buf, sizeof(buf)) && buf[0]) {
        free(cfg->storeUrl);
        cfg->storeUrl = strdup(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_FUNDER, buf, sizeof(buf)) && hex_to_bytes(buf, cfg->funder, 20) == 0) cfg->hasFunder = 1;
    for (int i = 0; i < N_MONEY_KNOBS; i++)
        if (get_uri_param(uri, gMoneyKnobs[i].uri, buf, sizeof(buf))) refuse_money_knob(i);
    if (get_uri_param(uri, HPPLITE_URI_RELAY_DEPOSIT, buf, sizeof(buf)))
        snprintf(gConfigError, sizeof(gConfigError), "relaydeposit= is gone (v0.8, #251): the relay is paid per call by x402 exact; drop it (HPPLITE_RELAY_CAP bounds a call)");
    if (get_uri_param(uri, HPPLITE_URI_SOFT, buf, sizeof(buf))) refuse_soft();
    if (get_uri_param(uri, HPPLITE_URI_SYNC_TO, buf, sizeof(buf)) && atoll(buf) > 0) cfg->syncTo = (uint64_t)atoll(buf);
    if (get_uri_param(uri, HPPLITE_URI_KEEP, buf, sizeof(buf))) { int64_t k = parse_keep_seconds(buf); if (k >= 0) cfg->keepSeconds = k; }
    if (get_uri_param(uri, HPPLITE_URI_FROM, buf, sizeof(buf))) refuse_from();
    if (get_uri_param(uri, HPPLITE_URI_SYNC_AT, buf, sizeof(buf)) && atoll(buf) > 0) cfg->syncAtMs = (uint64_t)atoll(buf);
    if (get_uri_param(uri, HPPLITE_URI_RELAY, buf, sizeof(buf)) && buf[0]) {
        free(cfg->relayUrl);
        cfg->relayUrl = strdup(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_MAX_TX_GAS, buf, sizeof(buf)) && atoll(buf) > 0) {
        cfg->maxTxGas = (uint64_t)atoll(buf);
    }

    /* Checkpoint interval */
    if (get_uri_param(uri, HPPLITE_URI_CHECKPOINT, buf, sizeof(buf)) && atoi(buf) > 0) {
        cfg->checkpointInterval = atoi(buf);
    }

    {   /* fleet= (#316): params only */
        static const char *names[] = { "fleet", "owner", "admit" };
        for (int i = 0; i < 3; i++) if (get_uri_param(uri, names[i], buf, sizeof(buf))) fleet_param(cfg, names[i], buf);
        static const char *given[7] = { HPPLITE_URI_CONTRACT, HPPLITE_URI_FACTORY, "anchor", HPPLITE_URI_CHECKPOINT, HPPLITE_URI_KEEP,
                                        HPPLITE_URI_SNAPSHOT, "anchorclose" };
        int uriGiven[7];
        for (int i = 0; i < 7; i++) uriGiven[i] = get_uri_param(uri, given[i], buf, sizeof(buf)) != NULL;
        fleet_check(cfg, uriGiven);
    }

    /* the chain's cadence applies to whatever goes to the chain (#162, #164): the batches
    ** of a node without a store, the anchors of a node with one */
    if (cfg->aliasIntervalMs && cfg->storeUrl) { if (!cfg->anchorGiven) cfg->anchorIntervalMs = cfg->aliasIntervalMs; }
    else if (cfg->aliasIntervalMs && !cfg->intervalGiven) cfg->batchIntervalMs = cfg->aliasIntervalMs;
    return 0;
}

int hpplite_config_load_sqlite_uri(HppliteConfig *cfg, const char *filename) {
    if (!cfg || !filename) return -1;

    const char *val;

    /* Set database path */
    free(cfg->dbPath);
    cfg->dbPath = strdup(filename);

    /* Role */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_ROLE);
    if (val) parse_role(cfg, val, 2);

    /* Chain ID (numeric) */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CHAIN_ID);
    if (val) {
        cfg->chainId = strtoull(val, NULL, 10);
        cfg->chainIdSource = 2;
    }

    /* L1 network alias; "local" is no chain at all, batches finalized here (#38) */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_L1);
    if (val && strcasecmp(val, "local") == 0) cfg->localOnly = 1;
    if (val && strncmp(val, "file://", 7) == 0) {
        /* a directory is the chain and the rollup (#35): no contract, no factory, no wallet needed */
        free(cfg->rpcUrl);
        cfg->rpcUrl = strdup(val);
        cfg->rpcUrlSource = 2;
        memset(cfg->contract, 0, 20);
        cfg->hasContract = 1;
        cfg->contractSource = 2;
    } else if (val && !cfg->localOnly) {
        uint64_t chainId = hpplite_da_resolve_alias(val);
        if (chainId > 0) {
            cfg->chainId = chainId;
            cfg->chainIdSource = 2;
            cfg->aliasIntervalMs = hpplite_network_batch_interval_ms(chainId);

            /* Also set RPC URL from alias */
            const char *rpc = hpplite_da_get_rpc_url(chainId);
            if (rpc) {
                free(cfg->rpcUrl);
                cfg->rpcUrl = strdup(rpc);
                cfg->rpcUrlSource = 2;
            }

            /* Also set factory from alias (explicit factory= below overrides) */
            if (!cfg->hasFactory) {
                const HppliteNetworkAlias *network = hpplite_da_lookup_network(val);
                if (network && network->factory
                    && hex_to_bytes(network->factory, cfg->factory, 20) == 0) {
                    cfg->hasFactory = 1;
                }
            }
        }
    }

    /* RPC URL */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_RPC_URL);
    if (val) {
        free(cfg->rpcUrl);
        cfg->rpcUrl = strdup(val);
        cfg->rpcUrlSource = 2;
    }

    /* Contract */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CONTRACT);
    if (val) {
        if (hpplite_config_parse_contract(cfg, val) == 0) {
            cfg->contractSource = 2;
        }
    }

    /* Factory */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_FACTORY);
    if (val) {
        if (hex_to_bytes(val, cfg->factory, 20) == 0) {
            cfg->hasFactory = 1;
        }
    }

    /* Private key */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_PRIVKEY);
    if (val) {
        hpplite_config_parse_privkey(cfg, val);
    }

    /* Private key file */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_PRIVKEY_FILE);
    if (val) {
        hpplite_config_load_privkey_file(cfg, val);
    }

    /* Keystore V3 (#17) */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_KEYSTORE);
    if (val) {
        hpplite_config_load_keystore(cfg, val, NULL);
    }

    /* Viewing key (#15) */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_VIEWKEY);
    if (val) {
        hpplite_config_parse_viewkey(cfg, val);
    }
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_OWNERKEY);
    if (val && val[0] && hpplite_config_parse_ownerkey(cfg, val) != 0)
        fprintf(stderr, "hpplite: ownerkey=: not a compressed public key (0x02/0x03 + 64 hex); ignored\n");

    /* Data dir */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_DATA_DIR);
    if (val) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(val);
    }


    /* Batch interval */
    val = sqlite3_uri_parameter(filename, "interval");
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) { cfg->batchIntervalMs = ms; cfg->intervalGiven = 1; }
    }
    val = sqlite3_uri_parameter(filename, "anchor");
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) { cfg->anchorIntervalMs = ms; cfg->anchorGiven = 1; }
    }
    val = sqlite3_uri_parameter(filename, "keeplease");
    if (val) cfg->keepLease = parse_on(val);
    val = sqlite3_uri_parameter(filename, "anchorclose");
    if (val) cfg->anchorClose = !parse_off(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_SNAPSHOT);
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) cfg->snapshotIntervalMs = ms;
    }

    /* Encryption */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_ENCRYPT);
    if (val) cfg->encrypt = parse_bool(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_COMPRESS);
    if (val) cfg->compress = parse_bool(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_DETERMINISM);
    if (val) refuse_determinism(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CAPTURE);
    if (val) refuse_capture(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_DBNAME);
    if (val && val[0]) { free(cfg->dbName); cfg->dbName = strdup(val); }
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_MAX_TX);
    if (val && atol(val) > 0) cfg->maxTxBytes = (size_t)atol(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_STORE);
    if (val && val[0]) { free(cfg->storeUrl); cfg->storeUrl = strdup(val); }
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_FUNDER);
    if (val && hex_to_bytes(val, cfg->funder, 20) == 0) cfg->hasFunder = 1;
    for (int i = 0; i < N_MONEY_KNOBS; i++)
        if (sqlite3_uri_parameter(filename, gMoneyKnobs[i].uri)) refuse_money_knob(i);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_SOFT);
    if (val) refuse_soft();
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_SYNC_TO);
    if (val && atoll(val) > 0) cfg->syncTo = (uint64_t)atoll(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_KEEP);
    if (val) { int64_t k = parse_keep_seconds(val); if (k >= 0) cfg->keepSeconds = k; }
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_FROM);
    if (val) refuse_from();
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_SYNC_AT);
    if (val && atoll(val) > 0) cfg->syncAtMs = (uint64_t)atoll(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_RELAY_DEPOSIT);
    if (val) snprintf(gConfigError, sizeof(gConfigError), "relaydeposit= is gone (v0.8, #251): the relay is paid per call by x402 exact; drop it (HPPLITE_RELAY_CAP bounds a call)");
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_RELAY);
    if (val && val[0]) { free(cfg->relayUrl); cfg->relayUrl = strdup(val); }
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_MAX_TX_GAS);
    if (val && atoll(val) > 0) cfg->maxTxGas = (uint64_t)atoll(val);

    /* Checkpoint interval */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CHECKPOINT);
    if (val && atoi(val) > 0) cfg->checkpointInterval = atoi(val);

    /* the chain's cadence applies to whatever goes to the chain (#162, #164): the batches
    ** of a node without a store, the anchors of a node with one */
    if (cfg->aliasIntervalMs && cfg->storeUrl) { if (!cfg->anchorGiven) cfg->anchorIntervalMs = cfg->aliasIntervalMs; }
    else if (cfg->aliasIntervalMs && !cfg->intervalGiven) cfg->batchIntervalMs = cfg->aliasIntervalMs;
    {   /* fleet= (#316): params only */
        static const char *names[] = { "fleet", "owner", "admit" };
        for (int i = 0; i < 3; i++) fleet_param(cfg, names[i], sqlite3_uri_parameter(filename, names[i]));
        static const char *given[7] = { HPPLITE_URI_CONTRACT, HPPLITE_URI_FACTORY, "anchor", HPPLITE_URI_CHECKPOINT, HPPLITE_URI_KEEP,
                                        HPPLITE_URI_SNAPSHOT, "anchorclose" };
        int uriGiven[7];
        for (int i = 0; i < 7; i++) uriGiven[i] = sqlite3_uri_parameter(filename, given[i]) != NULL;
        fleet_check(cfg, uriGiven);
    }
    return 0;
}

HppliteConfig *hpplite_config_load(const char *uri) {
    HppliteConfig *cfg = hpplite_config_create();
    if (!cfg) return NULL;

    /* Load from env */
    hpplite_config_load_env(cfg);

    /* Load from URI (overrides env) */
    if (uri) {
        hpplite_config_load_uri(cfg, uri);
    }

    return cfg;
}

int hpplite_config_load_privkey_file(HppliteConfig *cfg, const char *path) {
    struct stat st;
    FILE *f;
    char buf[160];
    size_t n;

    if (!cfg || !path) return -1;

    /* Like ssh: a key file readable by anyone else is not a key file.
    ** Checked on the open descriptor so the file cannot be swapped between
    ** the check and the read. */
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "hpplite: key file %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (fstat(fileno(f), &st) != 0) {
        fprintf(stderr, "hpplite: key file %s: %s\n", path, strerror(errno));
        fclose(f);
        return -1;
    }
    if (!S_ISREG(st.st_mode)) {
        fprintf(stderr, "hpplite: key file %s is not a regular file\n", path);
        fclose(f);
        return -1;
    }
    if (st.st_mode & (S_IRWXG | S_IRWXO)) {
        fprintf(stderr, "hpplite: key file %s is accessible by others (mode %03o); refusing to use it\n",
                path, (unsigned)(st.st_mode & 0777));
        fclose(f);
        return -1;
    }
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    /* first token: 0x-prefixed or bare hex, surrounding whitespace ignored */
    {
        char *z = buf, *e;
        while (*z == ' ' || *z == '\t' || *z == '\r' || *z == '\n') z++;
        e = z;
        while (*e && *e != ' ' && *e != '\t' && *e != '\r' && *e != '\n') e++;
        *e = '\0';
        if (hpplite_config_parse_privkey(cfg, z) != 0) {
            fprintf(stderr, "hpplite: key file %s does not contain a 32-byte hex key\n", path);
            memset(buf, 0, sizeof(buf));
            return -1;
        }
    }
    memset(buf, 0, sizeof(buf));
    return 0;
}

int hpplite_config_load_keystore(HppliteConfig *cfg, const char *path, const char *password) {
    char err[200];
    if (!cfg || !path) return -1;
    if (!password) password = getenv(HPPLITE_ENV_KEYSTORE_PASSWORD);
    if (hpplite_keystore_load(path, password, cfg->privkey, err, sizeof err) != 0) {
        memset(cfg->privkey, 0, sizeof cfg->privkey);
        fprintf(stderr, "hpplite: keystore %s: %s\n", path, err);
        return -1;
    }
    cfg->hasPrivkey = 1;
    return 0;
}

/* store=on and relay=on (#192): the network's canonical operators, once every source of settings is read and the
** chain is known; an error when the network has none (a node asked for a store must not quietly post on chain) */
int hpplite_config_resolve_operators(HppliteConfig *cfg) {
    struct { char **pz; const char *(*url)(uint64_t); const char *what; } k[2] = {
        { &cfg->storeUrl, hpplite_network_store_url, "store" }, { &cfg->relayUrl, hpplite_network_relay_url, "relay" } };
    int i;
    for (i = 0; i < 2; i++) {
        if (!*k[i].pz || strcasecmp(*k[i].pz, "on") != 0) continue;
        const char *url = cfg->chainId ? k[i].url(cfg->chainId) : NULL;
        if (!url) {
            snprintf(gConfigError, sizeof(gConfigError), "%s=on: no canonical %s for chain %llu (give its URL)", k[i].what, k[i].what,
                     (unsigned long long)cfg->chainId);
            return -1;
        }
        free(*k[i].pz);
        *k[i].pz = strdup(url);
        if (i == 1 && cfg->relayCap == 0) cfg->relayCap = HPPLITE_DEFAULT_RELAY_CAP;   /* relay=on pays: a cap of 0 could pay nothing (#251) */
    }
    if (cfg->hasFleet && !gConfigError[0]) {
        /* the dbId said at once, so a wrong owner= or dbname= is seen (#316) */
        char id[65], fl[41], ow[41];
        for (i = 0; i < 32; i++) snprintf(id + 2 * i, 3, "%02x", cfg->dbId[i]);
        for (i = 0; i < 20; i++) { snprintf(fl + 2 * i, 3, "%02x", cfg->fleet[i]); snprintf(ow + 2 * i, 3, "%02x", cfg->fleetOwner[i]); }
        fprintf(stderr, "hpplite: fleet 0x%s: database \"%s\" of 0x%s, dbId 0x%s\n", fl, cfg->dbName, ow, id);
        /* until the node's fleet mode is built (#316 b-e), a database in a fleet does not open as anything else */
        snprintf(gConfigError, sizeof(gConfigError), "fleet=: the node's fleet mode is not served yet (#316); this database does not open as a rollup instead");
        return -1;
    }
    return 0;
}

int hpplite_config_parse_viewkey(HppliteConfig *cfg, const char *hex) {
    if (!cfg || !hex) return -1;
    if (hex_to_bytes(hex, cfg->viewkey, 32) == 0) { cfg->hasViewKey = 1; return 0; }
    return -1;
}

int hpplite_config_parse_ownerkey(HppliteConfig *cfg, const char *hex) {
    if (!cfg || !hex) return -1;
    if (hex_to_bytes(hex, cfg->ownerPub, 33) == 0 && (cfg->ownerPub[0] == 2 || cfg->ownerPub[0] == 3)) { cfg->hasOwnerPub = 1; return 0; }
    cfg->hasOwnerPub = 0;
    return -1;
}

int hpplite_config_parse_privkey(HppliteConfig *cfg, const char *hex) {
    if (!cfg || !hex) return -1;

    if (hex_to_bytes(hex, cfg->privkey, 32) == 0) {
        cfg->hasPrivkey = 1;
        return 0;
    }
    return -1;
}

int hpplite_config_parse_contract(HppliteConfig *cfg, const char *hex) {
    if (!cfg || !hex) return -1;

    if (hex_to_bytes(hex, cfg->contract, 20) == 0) {
        cfg->hasContract = 1;
        return 0;
    }
    return -1;
}

static const char *source_str(int source) {
    switch (source) {
        case 1: return "env";
        case 2: return "uri";
        default: return "default";
    }
}

void hpplite_config_print(const HppliteConfig *cfg) {
    if (!cfg) return;

    printf("HPPLite Configuration:\n");
    printf("  nodeId:     %s\n", cfg->nodeId ? cfg->nodeId : "(auto)");
    printf("  role:       %s (%s)\n", cfg->roleVerify ? "verify" : "-", source_str(cfg->roleSource));
    printf("  chainId:    %llu (%s)\n", (unsigned long long)cfg->chainId, source_str(cfg->chainIdSource));
    printf("  rpcUrl:     %s (%s)\n", cfg->rpcUrl, source_str(cfg->rpcUrlSource));
    printf("  contract:   %s", cfg->hasContract ? "0x" : "(not set)");
    if (cfg->hasContract) {
        for (int i = 0; i < 20; i++) printf("%02x", cfg->contract[i]);
        printf(" (%s)", source_str(cfg->contractSource));
    }
    printf("\n");
    printf("  dataDir:    %s\n", cfg->dataDir);
    printf("  dbPath:     %s\n", cfg->dbPath ? cfg->dbPath : "(not set)");
    printf("  privkey:    %s\n", cfg->hasPrivkey ? "(set)" : "(not set)");
    printf("  interval:   %dms\n", cfg->batchIntervalMs);
    printf("  encrypt:    %s\n", cfg->encrypt ? "on" : "off");
}
