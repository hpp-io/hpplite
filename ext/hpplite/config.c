/*
** HPPLite Configuration Implementation
**
** Layered config: Defaults → Env vars → URI params
*/

#include "config.h"
#include <sys/stat.h>
#include <errno.h>
#include "da_uri.h"
#include "hpplite_sqlite.h"
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

HppliteConfigRole hpplite_config_parse_role(const char *str) {
    if (!str) return HPPLITE_CFG_ROLE_UNKNOWN;
    if (strcmp(str, "sequencer") == 0) return HPPLITE_CFG_ROLE_SEQUENCER;
    if (strcmp(str, "observer") == 0) return HPPLITE_CFG_ROLE_OBSERVER;
    if (strcmp(str, "verify") == 0) return HPPLITE_CFG_ROLE_VERIFY;
    return HPPLITE_CFG_ROLE_UNKNOWN;
}

const char *hpplite_config_role_str(HppliteConfigRole role) {
    switch (role) {
        case HPPLITE_CFG_ROLE_SEQUENCER: return "sequencer";
        case HPPLITE_CFG_ROLE_OBSERVER: return "observer";
        case HPPLITE_CFG_ROLE_VERIFY: return "verify";
        default: return "unknown";
    }
}

static char gConfigError[160];
const char *hpplite_config_error(void) { return gConfigError[0] ? gConfigError : NULL; }

/* capture=rows|text, nothing else: a misspelling must not quietly choose (#80) */
static void parse_capture(HppliteConfig *cfg, const char *val, int source) {
    if (strcasecmp(val, "rows") == 0) cfg->captureText = 0;
    else if (strcasecmp(val, "text") == 0) cfg->captureText = 1;
    else { snprintf(gConfigError, sizeof(gConfigError), "capture=%.40s is neither rows nor text", val); return; }
    cfg->captureSource = source;
}

HppliteConfig *hpplite_config_create(void) {
    HppliteConfig *cfg = calloc(1, sizeof(HppliteConfig));
    if (!cfg) return NULL;
    gConfigError[0] = 0;

    /* Set defaults */
    cfg->chainId = HPPLITE_DEFAULT_CHAIN_ID;
    cfg->rpcUrl = strdup(HPPLITE_DEFAULT_RPC_URL);
    cfg->dataDir = strdup(HPPLITE_DEFAULT_DATA_DIR);
    cfg->role = HPPLITE_CFG_ROLE_OBSERVER;  /* Safe default */
    cfg->batchIntervalMs = 5000;  /* 5 seconds default */
    cfg->encrypt = 1;             /* everything posted is public otherwise */

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
    free(cfg->dbName);
    free(cfg);
}

int hpplite_config_load_env(HppliteConfig *cfg) {
    if (!cfg) return -1;

    const char *val;

    /* Role */
    val = getenv(HPPLITE_ENV_ROLE);
    if (val) {
        cfg->role = hpplite_config_parse_role(val);
        cfg->roleSource = 1;
    }

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

    /* Data dir */
    val = getenv(HPPLITE_ENV_DATA_DIR);
    if (val) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(val);
    }

    /* Node ID */
    val = getenv(HPPLITE_ENV_NODE_ID);
    if (val) {
        free(cfg->nodeId);
        cfg->nodeId = strdup(val);
    }

    /* Batch interval */
    val = getenv(HPPLITE_ENV_BATCH_INTERVAL);
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) cfg->batchIntervalMs = ms;
    }

    /* Encryption */
    val = getenv(HPPLITE_ENV_ENCRYPT);
    if (val) cfg->encrypt = parse_bool(val);

    /* Determinism: strict (default) or warn */
    val = getenv(HPPLITE_ENV_DETERMINISM);
    if (val) cfg->determinismWarn = (strcasecmp(val, "warn") == 0);
    val = getenv(HPPLITE_ENV_CAPTURE);
    if (val) parse_capture(cfg, val, 1);
    val = getenv(HPPLITE_ENV_DBNAME);
    if (val && val[0]) { free(cfg->dbName); cfg->dbName = strdup(val); }
    val = getenv(HPPLITE_ENV_STORE);
    if (val && val[0]) { free(cfg->storeUrl); cfg->storeUrl = strdup(val); }
    val = getenv(HPPLITE_ENV_STORE_CAP);
    if (val && atoll(val) > 0) cfg->storeCap = (uint64_t)atoll(val);
    val = getenv(HPPLITE_ENV_MAX_TX);
    if (val && atol(val) > 0) cfg->maxTxBytes = (size_t)atol(val);
    val = getenv(HPPLITE_ENV_MAX_TX_GAS);
    if (val && atoll(val) > 0) cfg->maxTxGas = (uint64_t)atoll(val);

    /* Checkpoint interval */
    val = getenv(HPPLITE_ENV_CHECKPOINT);
    if (val && atoi(val) > 0) cfg->checkpointInterval = atoi(val);

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
    if (get_uri_param(uri, HPPLITE_URI_ROLE, buf, sizeof(buf))) {
        cfg->role = hpplite_config_parse_role(buf);
        cfg->roleSource = 2;
    }

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

    /* Data dir */
    if (get_uri_param(uri, HPPLITE_URI_DATA_DIR, buf, sizeof(buf))) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(buf);
    }

    /* Node ID */
    if (get_uri_param(uri, HPPLITE_URI_NODE_ID, buf, sizeof(buf))) {
        free(cfg->nodeId);
        cfg->nodeId = strdup(buf);
    }

    /* Batch interval */
    if (get_uri_param(uri, "interval", buf, sizeof(buf))) {
        int ms = parse_duration_ms(buf);
        if (ms >= 0) cfg->batchIntervalMs = ms;
    }

    /* Encryption */
    if (get_uri_param(uri, HPPLITE_URI_ENCRYPT, buf, sizeof(buf))) {
        cfg->encrypt = parse_bool(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_DETERMINISM, buf, sizeof(buf))) {
        cfg->determinismWarn = (strcasecmp(buf, "warn") == 0);
    }
    if (get_uri_param(uri, HPPLITE_URI_CAPTURE, buf, sizeof(buf))) parse_capture(cfg, buf, 2);
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
    if (get_uri_param(uri, HPPLITE_URI_STORE_CAP, buf, sizeof(buf)) && atoll(buf) > 0) {
        cfg->storeCap = (uint64_t)atoll(buf);
    }
    if (get_uri_param(uri, HPPLITE_URI_MAX_TX_GAS, buf, sizeof(buf)) && atoll(buf) > 0) {
        cfg->maxTxGas = (uint64_t)atoll(buf);
    }

    /* Checkpoint interval */
    if (get_uri_param(uri, HPPLITE_URI_CHECKPOINT, buf, sizeof(buf)) && atoi(buf) > 0) {
        cfg->checkpointInterval = atoi(buf);
    }

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
    if (val) {
        cfg->role = hpplite_config_parse_role(val);
        cfg->roleSource = 2;
    }

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

    /* Data dir */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_DATA_DIR);
    if (val) {
        free(cfg->dataDir);
        cfg->dataDir = strdup(val);
    }

    /* Node ID */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_NODE_ID);
    if (val) {
        free(cfg->nodeId);
        cfg->nodeId = strdup(val);
    }

    /* Batch interval */
    val = sqlite3_uri_parameter(filename, "interval");
    if (val) {
        int ms = parse_duration_ms(val);
        if (ms >= 0) cfg->batchIntervalMs = ms;
    }

    /* Encryption */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_ENCRYPT);
    if (val) cfg->encrypt = parse_bool(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_DETERMINISM);
    if (val) cfg->determinismWarn = (strcasecmp(val, "warn") == 0);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CAPTURE);
    if (val) parse_capture(cfg, val, 2);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_DBNAME);
    if (val && val[0]) { free(cfg->dbName); cfg->dbName = strdup(val); }
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_MAX_TX);
    if (val && atol(val) > 0) cfg->maxTxBytes = (size_t)atol(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_STORE);
    if (val && val[0]) { free(cfg->storeUrl); cfg->storeUrl = strdup(val); }
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_STORE_CAP);
    if (val && atoll(val) > 0) cfg->storeCap = (uint64_t)atoll(val);
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_MAX_TX_GAS);
    if (val && atoll(val) > 0) cfg->maxTxGas = (uint64_t)atoll(val);

    /* Checkpoint interval */
    val = sqlite3_uri_parameter(filename, HPPLITE_URI_CHECKPOINT);
    if (val && atoi(val) > 0) cfg->checkpointInterval = atoi(val);

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
    printf("  role:       %s (%s)\n", hpplite_config_role_str(cfg->role), source_str(cfg->roleSource));
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
