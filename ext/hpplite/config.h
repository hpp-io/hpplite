/*
** HPPLite Configuration
**
** Layered configuration system with priority:
**   URI params > Env vars > Defaults
**
** URI format:
**   file:state.db?hpplite=on&role=sequencer&l1=hpp-sepolia&contract=0x...
**
** Environment variables (same names as .env / run_integration.sh):
**   HPPLITE_PRIVATE_KEY       - Private key (0x + 64 hex)
**   HPPLITE_PRIVATE_KEY_FILE  - Path to a file containing the private key
**   HPPLITE_L1_RPC            - L1 RPC endpoint
**   HPPLITE_L1_CHAIN_ID       - L1 chain ID (e.g., 181228)
**   HPPLITE_FACTORY_ADDRESS   - Factory contract (auto-creates rollup per wallet)
**   HPPLITE_CONTRACT_ADDRESS  - Existing rollup contract
**   HPPLITE_DATA_DIR          - Local data directory
**   HPPLITE_BATCH_INTERVAL    - Batch flush interval (500ms, 5s, 1m, 1h; 0 = no timer)
**   HPPLITE_ENCRYPT           - on|off: seal batches before posting (default on)
**   HPPLITE_CHECKPOINT_INTERVAL - batches per checkpoint / state root (default 100)
**   HPPLITE_DETERMINISM       - strict (default: refuse SQL replay cannot reproduce) | warn
**   HPPLITE_CAPTURE           - rows (default: what each write changed, #19) | text (each write statement, v0.1)
**   HPPLITE_DBNAME            - a name: the database gets its own rollup among the wallet's (#61); {file} = its file name
**   HPPLITE_ROLE              - sequencer|observer
**   HPPLITE_NODE_ID           - Node identifier (status output only)
*/

#ifndef HPPLITE_CONFIG_H
#define HPPLITE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Environment variable names
*/
#define HPPLITE_ENV_ROLE          "HPPLITE_ROLE"
#define HPPLITE_ENV_CHAIN_ID      "HPPLITE_L1_CHAIN_ID"
#define HPPLITE_ENV_RPC_URL       "HPPLITE_L1_RPC"
#define HPPLITE_ENV_CONTRACT      "HPPLITE_CONTRACT_ADDRESS"
#define HPPLITE_ENV_FACTORY       "HPPLITE_FACTORY_ADDRESS"
#define HPPLITE_ENV_PRIVKEY       "HPPLITE_PRIVATE_KEY"
#define HPPLITE_ENV_PRIVKEY_FILE  "HPPLITE_PRIVATE_KEY_FILE"
#define HPPLITE_ENV_KEYSTORE      "HPPLITE_KEYSTORE"
#define HPPLITE_ENV_VIEWKEY       "HPPLITE_VIEW_KEY"
#define HPPLITE_ENV_OWNERKEY      "HPPLITE_OWNER_KEY"
#define HPPLITE_ENV_KEYSTORE_PASSWORD "HPPLITE_KEYSTORE_PASSWORD"
#define HPPLITE_ENV_DATA_DIR      "HPPLITE_DATA_DIR"
#define HPPLITE_ENV_NODE_ID       "HPPLITE_NODE_ID"
#define HPPLITE_ENV_BATCH_INTERVAL "HPPLITE_BATCH_INTERVAL"
#define HPPLITE_ENV_ENCRYPT       "HPPLITE_ENCRYPT"
#define HPPLITE_ENV_COMPRESS      "HPPLITE_COMPRESS"       /* off: the batch JSON as is (#12) */
#define HPPLITE_ENV_CHECKPOINT    "HPPLITE_CHECKPOINT_INTERVAL"
#define HPPLITE_ENV_DETERMINISM   "HPPLITE_DETERMINISM"
#define HPPLITE_ENV_CAPTURE       "HPPLITE_CAPTURE"
#define HPPLITE_ENV_DBNAME        "HPPLITE_DBNAME"
#define HPPLITE_ENV_MAX_TX        "HPPLITE_MAX_TX_BYTES"
#define HPPLITE_ENV_MAX_TX_GAS    "HPPLITE_MAX_TX_GAS"
#define HPPLITE_ENV_STORE         "HPPLITE_STORE"          /* where batch bytes live instead of the chain (#11) */
#define HPPLITE_ENV_STORE_CAP     "HPPLITE_STORE_CAP"
#define HPPLITE_ENV_STORE_PERMIT "HPPLITE_STORE_PERMIT"
#define HPPLITE_ENV_SPEND_CAP     "HPPLITE_SPEND_CAP"
#define HPPLITE_ENV_RELAY_DEPOSIT "HPPLITE_RELAY_DEPOSIT"
#define HPPLITE_ENV_SOFT          "HPPLITE_SOFT"
#define HPPLITE_ENV_LOW_BALANCE   "HPPLITE_LOW_BALANCE"
#define HPPLITE_ENV_RELAY         "HPPLITE_RELAY"          /* a relay submits the signed forms of the writes, paid over x402 (#126) */
#define HPPLITE_ENV_RELAY_CAP     "HPPLITE_RELAY_CAP"

/*
** URI parameter names (same as env vars but lowercase, no prefix)
*/
#define HPPLITE_URI_ENABLED       "hpplite"
#define HPPLITE_URI_ROLE          "role"
#define HPPLITE_URI_CHAIN_ID      "chainid"
#define HPPLITE_URI_L1            "l1"          /* Alias for chain (hpp-sepolia, etc) */
#define HPPLITE_URI_RPC_URL       "rpc"
#define HPPLITE_URI_CONTRACT      "contract"
#define HPPLITE_URI_PRIVKEY       "privkey"
#define HPPLITE_URI_PRIVKEY_FILE  "keyfile"
#define HPPLITE_URI_KEYSTORE      "keystore"
#define HPPLITE_URI_OWNERKEY      "ownerkey"      /* the rollup owner's public key (33 bytes, compressed): every batch sealed to it too (#209) */
#define HPPLITE_URI_VIEWKEY       "viewkey"       /* a viewing key's private key (#15) */     /* an Ethereum keystore V3 file (#17); passphrase from HPPLITE_KEYSTORE_PASSWORD */
#define HPPLITE_URI_DATA_DIR      "datadir"
#define HPPLITE_URI_ENCRYPT       "encrypt"
#define HPPLITE_URI_COMPRESS      "compress"    /* on (default): deflate the batch when it shrinks; off: as is (#12) */
#define HPPLITE_URI_CHECKPOINT    "checkpoint"
#define HPPLITE_URI_DETERMINISM   "determinism"
#define HPPLITE_URI_CAPTURE       "capture"
#define HPPLITE_URI_DBNAME        "dbname"
#define HPPLITE_URI_MAX_TX        "maxtx"
#define HPPLITE_URI_MAX_TX_GAS    "maxtxgas"
#define HPPLITE_URI_NODE_ID       "nodeid"
#define HPPLITE_URI_FACTORY       "factory"
#define HPPLITE_URI_STORE         "store"       /* store=http://host:port: a pin service holds the bytes, the chain the commitment (#11) */
#define HPPLITE_URI_STORE_CAP     "storecap"    /* the most to pay the store per call, atomic units of its asset */
#define HPPLITE_URI_STORE_PERMIT "storepermit"  /* hpplite-store/2 (#214): what the funding wallet's permit lets the store pull; 0: none */
#define HPPLITE_URI_SYNC_TO      "syncto"      /* a verifier's stop: no batch past this height is applied (hpplite-verify --to, #206) */
#define HPPLITE_URI_SOFT          "soft"        /* a verifier with a store follows the soft tier (#166); off: the anchored height only */
#define HPPLITE_URI_RELAY_DEPOSIT "relaydeposit" /* batch-settlement for the relay (#179): the most unspent in its channel */
#define HPPLITE_URI_SPEND_CAP     "spendcap"    /* the most paid to the store and the relay per hour or day (#173): N/hour, N/day */
#define HPPLITE_URI_LOW_BALANCE   "lowbalance"  /* warn when the money left lasts fewer hours than this at the recent rate (#173) */
#define HPPLITE_URI_RELAY         "relay"       /* relay=http://host:port: the writes go to the chain through it, no ETH needed (#126) */
#define HPPLITE_URI_RELAY_CAP     "relaycap"    /* the most to pay the relay per call, atomic units of its asset */
#define HPPLITE_URI_STORE_FALLBACK "storefallback"   /* off: a batch waits while the store is down; on (default): the chain carries it */

/*
** Default values
*/
#define HPPLITE_DEFAULT_CHAIN_ID   181228
#define HPPLITE_DEFAULT_RPC_URL    "https://sepolia.hpp.io"
#define HPPLITE_DEFAULT_DATA_DIR   "/tmp/hpplite"

/*
** Role enum
*/
typedef enum {
    HPPLITE_CFG_ROLE_UNKNOWN = 0,
    HPPLITE_CFG_ROLE_SEQUENCER,
    HPPLITE_CFG_ROLE_OBSERVER,
    HPPLITE_CFG_ROLE_VERIFY          /* rebuild and compare only: never post, claim or create (#5) */
} HppliteConfigRole;

/*
** Unified configuration struct
*/
typedef struct HppliteConfig {
    /* Node identity */
    char *nodeId;                    /* Node identifier */
    HppliteConfigRole role;          /* sequencer|observer */
    unsigned char privkey[32];       /* Node private key */
    int hasPrivkey;                  /* Whether privkey is set */
    unsigned char viewkey[32];       /* A viewing key (#15): reads batches sealed to its public key, signs nothing */
    int hasViewKey;
    unsigned char ownerPub[33];      /* ownerkey= (#209): the owner's public key, a recipient of every batch */
    int hasOwnerPub;

    /* L1 connection */
    uint64_t chainId;                /* Chain ID */
    char *rpcUrl;                    /* RPC endpoint */
    unsigned char contract[20];      /* L1 contract address */
    int hasContract;                 /* Whether contract is set */
    unsigned char factory[20];       /* Factory contract address */
    int hasFactory;                  /* Whether factory is set */

    /* Paths */
    char *dataDir;                   /* Data directory */
    char *dbPath;                    /* Database path (from URI) */

    /* Batching */
    int batchIntervalMs;             /* Batch interval in milliseconds */
    int encrypt;                     /* Seal batches before posting (default 1) */
    int compress;                    /* Deflate batches before sealing or posting when it shrinks them (default 1, #12) */
    int checkpointInterval;          /* Batches per checkpoint (0 = node default, 100) */
    int determinismWarn;             /* 1: only warn about non-deterministic SQL (default 0: refuse it) */
    int captureText;                 /* 1: capture statements, as v0.1 (default 0: rows, #19) */
    int capturePages;                /* 1: capture pages through the VFS (#146); captureText is then 0 */
    int captureSource;               /* ...where from: 0 default, 1 environment, 2 URI (the URI wins, #80) */
    int captureAuto;                 /* no capture given (the default, #146): pages for a new rollup, the history's mode for an existing one */
    int intervalGiven;               /* interval= was said: a chain alias's own cadence does not apply */
    int anchorIntervalMs;            /* anchor=: how often a store node anchors its batches on the chain (#164); default 30 min, the chain's cadence */
    int anchorGiven;
    int aliasIntervalMs;             /* the cadence of the chain l1= named (0: none) */
    int localOnly;                   /* l1=local: finalize batches locally and keep no history (#38) */
    char *dbName;                    /* Its own rollup among the wallet's, by this name (#61);
                                     ** "{file}": the database file's name */
    size_t maxTxBytes;               /* Largest batch per transaction (0 = by chain id, else unknown) */
    uint64_t maxTxGas;               /* Gas cap per transaction (0 = by chain id, else unknown) */
    char *storeUrl;                  /* store=: the batch bytes' store (#11); NULL = on chain */
    char *relayUrl;                  /* relay=: the signed forms of the writes go through it (#126); NULL = the wallet sends them */
    uint64_t relayCap;               /* relaycap=: the most to pay the relay per call (0 = pay nothing) */
    uint64_t storeCap;               /* storecap=: the most to pay the store per call (0 = pay nothing) */
    uint64_t storePermit;            /* storepermit=: what the permit lets the store pull, atomic units (#214) */
    int storePermitSet;              /* ...given (0 is a value: no permit, the 402s paid) */
    int softOff;                     /* soft=off: a verifier stays at the anchored height (#166) */
    uint64_t syncTo;                 /* syncto=: a verifier applies no batch past this height (#206); 0: none */
    uint64_t relayDeposit;           /* relaydeposit=: the relay paid by voucher, the most unspent in its channel (0 = exact, #179) */
    uint64_t spendCap, spendWindow;  /* spendcap=: the most paid in the window (seconds), store and relay together (0 = none, #173) */
    int lowBalanceHours;             /* lowbalance=: warn below this many hours of money at the recent rate (0 = the default, 72; -1 = off) */
    int storeFallback;               /* storefallback=off keeps a batch waiting while the store is down; on (default): the chain carries it */

    /* Source tracking (for debugging) */
    int roleSource;                  /* 0=default, 1=env, 2=uri */
    int chainIdSource;
    int rpcUrlSource;
    int contractSource;
} HppliteConfig;

/*
** Create config with defaults.
** Caller must free with hpplite_config_free().
*/
HppliteConfig *hpplite_config_create(void);

/*
** Free config and all allocated strings.
*/
void hpplite_config_free(HppliteConfig *cfg);

/* A value the last load refused (capture= not rows or text), or NULL: an
** open with such a setting fails rather than runs with another (#80) */
const char *hpplite_config_error(void);

/*
** Load config from environment variables.
** Overrides current values if env vars are set.
*/
int hpplite_config_load_env(HppliteConfig *cfg);

/*
** Load config from SQLite URI parameters.
** Call after sqlite3_open() with the db handle.
** Overrides current values if URI params are set.
*/
int hpplite_config_load_uri(HppliteConfig *cfg, const char *uri);

/*
** Load config from SQLite db filename using sqlite3_uri_parameter().
** This is the correct way to load config from an open database,
** since sqlite3_db_filename() only returns the path, not the URI.
*/
int hpplite_config_load_sqlite_uri(HppliteConfig *cfg, const char *filename);

/*
** Load config from all sources (defaults + env + uri).
** Convenience function that calls the above in order.
*/
HppliteConfig *hpplite_config_load(const char *uri);

/*
** Parse role string to enum.
*/
HppliteConfigRole hpplite_config_parse_role(const char *str);

/*
** Get role string from enum.
*/
const char *hpplite_config_role_str(HppliteConfigRole role);

/*
** Load private key from file.
** Returns 0 on success, -1 on error.
*/
int hpplite_config_load_privkey_file(HppliteConfig *cfg, const char *path);
/* An Ethereum keystore V3 file (#17); password NULL: HPPLITE_KEYSTORE_PASSWORD. 0 keyed, -1 (said on stderr) */
int hpplite_config_load_keystore(HppliteConfig *cfg, const char *path, const char *password);

/*
** Parse hex private key string.
** Returns 0 on success, -1 on error.
*/
int hpplite_config_parse_privkey(HppliteConfig *cfg, const char *hex);
int hpplite_config_parse_viewkey(HppliteConfig *cfg, const char *hex);   /* (#15) */
int hpplite_config_parse_ownerkey(HppliteConfig *cfg, const char *hex);  /* (#209) */
int hpplite_config_resolve_operators(HppliteConfig *cfg);                 /* store=on, relay=on (#192) */

/*
** Parse hex contract address string.
** Returns 0 on success, -1 on error.
*/
int hpplite_config_parse_contract(HppliteConfig *cfg, const char *hex);

/*
** Print config for debugging.
*/
void hpplite_config_print(const HppliteConfig *cfg);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_CONFIG_H */
