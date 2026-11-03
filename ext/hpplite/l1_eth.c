/*
** HPPLite Ethereum L1 Client
**
** Real L1 client implementation using eth_client.
** Implements l1_interface.h for production use with HPP Sepolia.
*/

#include "l1_backend.h"
#include "hpplite_sqlite.h"
#include "eth_client.h"
#include "abi.h"
#include "keccak256.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>

typedef struct EthL1 {
    HppliteL1 base;               /* the function table first: l1.c dispatches through it */
    EthClient *eth;
    uint8_t contract[20];      /* Main HPPLite contract */
    uint8_t da_contract[20];   /* HPPLiteDA contract (for on-chain DA) */
    uint64_t version;          /* Coordinator VERSION, learned from getSystemConfig() */
    int has_da_contract;
    uint8_t privkey[32];
    uint8_t address[20];       /* the wallet: from the key, or the signer's (#17) */
    int has_privkey;

    int lastOversized;         /* the last batch submission was too large for the chain */
    uint64_t maxTxGas;         /* the chain's gas cap per transaction (0 = unknown) */

    /* Network info */
    uint64_t chainId;
    char *rpcUrl;

    /* Cached state */
    HppliteL1State *cached_state;
    HppliteSystemConfig *cached_system_config;

    /* Callback */
    HppliteL1Callback callback;
    void *callback_arg;

    /* DA v3 (#164, coordinator VERSION >= 9): where each height's commitment
    ** is, learned by walking the events backward from lastBlock(): every
    ** event names the block of the commitment before it. Heights 1..indexedHeight. */
    uint64_t *aBlock;          /* [height]: the block whose log carries the commitment */
    uint64_t *aRangeFrom;      /* [height]: the first height of the range the height is inside, 0 for a per-batch commitment */
    uint64_t nIndexAlloc;
    uint64_t indexedHeight;
    uint64_t lastIndexedBlock; /* the block the last walk started from: the next walk stops there */
} EthL1;

/* The backend's view of the l1 parameter */
#define self ((EthL1 *)l1)

/* Function selectors - HPPLite contract (coordination) */
static uint8_t SEL_GET_STATE[4];
static uint8_t SEL_SET_CHECKPOINT_INTERVAL[4];
static uint8_t SEL_CLAIM_SEQUENCER[4];
static uint8_t SEL_RENEW_LEASE[4];
static uint8_t SEL_SUBMIT_CHECKPOINT[4];
static uint8_t SEL_IS_LEASE_ACTIVE[4];
static uint8_t SEL_GET_SYSTEM_CONFIG[4];

/* Function selectors - HPPLiteDA contract (data availability) */
static uint8_t SEL_SUBMIT_BATCH[4];
static uint8_t SEL_GET_BATCH[4];        /* DA v1 (coordinator VERSION < 8): the bytes in storage */
static uint8_t SEL_GET_BATCH_HASH[4];   /* DA v1 */
static uint8_t SEL_GET_DA_STATE[4];
static uint8_t SEL_BLOCK_OF[4];         /* DA v2 (#33): the block whose log carries a height */
static uint8_t SEL_COMMIT_BATCH[4];     /* DA v2 (#11): commitBatch(bytes32,uint256,bytes32,bytes,bytes) */
static uint8_t TOPIC_BATCH_SUBMITTED[32];   /* DA v2: BatchSubmitted(uint256 indexed height, bytes32 indexed hash, bytes data) */
static uint8_t TOPIC_BATCH_COMMITTED[32];   /* DA v2: BatchCommitted(uint256 indexed height, bytes32 indexed hash, bytes ref, bytes receipt) */
static uint8_t SEL_COMMIT_RANGE[4];     /* DA v3 (#164): commitRange(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes) */
static uint8_t SEL_LAST_BLOCK[4];       /* DA v3: lastBlock() */
static uint8_t TOPIC3_BATCH_SUBMITTED[32];  /* DA v3: BatchSubmitted(uint256 indexed height, bytes32 indexed hash, bytes data, uint256 prevBlock) */
static uint8_t TOPIC3_BATCH_COMMITTED[32];  /* DA v3: BatchCommitted(..., bytes ref, bytes receipt, uint256 prevBlock) */
static uint8_t TOPIC3_RANGE_COMMITTED[32];  /* DA v3: RangeCommitted(uint256 indexed from, uint256 indexed to, bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest, bytes receipt, uint256 prevBlock) */

/* Function selectors - HPPLiteFactory contract */
static uint8_t SEL_GET_ROLLUP[4];
static uint8_t SEL_HAS_ROLLUP[4];
static uint8_t SEL_FORCE_EXPIRE_LEASE[4];
static uint8_t SEL_SET_SEQUENCER[4];        /* setSequencer(address): the owner rotates the hot key (#17) */
static uint8_t SEL_RELEASE_LEASE[4];
static uint8_t SEL_GET_OR_CREATE_ROLLUP[4];
static uint8_t SEL_GET_ROLLUPS_BY_OWNER[4];
static uint8_t SEL_CREATE_ROLLUP[4];
static uint8_t SEL_CREATE_SIMPLE_ROLLUP[4];

static int selectors_initialized = 0;
static int eth_get_da_state(HppliteL1 *l1, uint64_t *last_batch_height, uint64_t *total_batches, uint8_t latest_batch_hash[32]);

/* Default factory address on HPP Sepolia (update after deployment) */
static const char *DEFAULT_FACTORY_ADDRESS = "0x0000000000000000000000000000000000000000";

/* Chain id for signing: was hard-coded to HPP Sepolia in three places, so
** chainid= never reached the client and no other chain (anvil, 31337)
** could be used. Found by the anvil harness (#47). */

static void init_selectors(void) {
    if (selectors_initialized) return;

    /* HPPLite selectors (coordination) */
    abi_function_selector("getState()", SEL_GET_STATE);
    abi_function_selector("setCheckpointInterval(uint256)", SEL_SET_CHECKPOINT_INTERVAL);
    abi_function_selector("claimSequencer(bytes32)", SEL_CLAIM_SEQUENCER);
    abi_function_selector("renewLease(bytes32)", SEL_RENEW_LEASE);
    abi_function_selector("submitCheckpoint(bytes32,uint256,uint256,bytes32)", SEL_SUBMIT_CHECKPOINT);
    abi_function_selector("isLeaseActive()", SEL_IS_LEASE_ACTIVE);
    abi_function_selector("forceExpireLease()", SEL_FORCE_EXPIRE_LEASE);
    abi_function_selector("setSequencer(address)", SEL_SET_SEQUENCER);
    abi_function_selector("releaseLease(bytes32)", SEL_RELEASE_LEASE);
    abi_function_selector("getSystemConfig()", SEL_GET_SYSTEM_CONFIG);

    /* HPPLiteDA selectors (data availability) */
    abi_function_selector("submitBatch(bytes32,uint256,bytes)", SEL_SUBMIT_BATCH);
    abi_function_selector("getBatch(uint256)", SEL_GET_BATCH);
    abi_function_selector("getBatchHash(uint256)", SEL_GET_BATCH_HASH);
    abi_function_selector("getDAState()", SEL_GET_DA_STATE);
    abi_function_selector("blockOf(uint256)", SEL_BLOCK_OF);
    abi_function_selector("commitBatch(bytes32,uint256,bytes32,bytes,bytes)", SEL_COMMIT_BATCH);
    {
        static const char zSig[] = "BatchSubmitted(uint256,bytes32,bytes)";
        static const char zCommit[] = "BatchCommitted(uint256,bytes32,bytes,bytes)";
        keccak256((const uint8_t *)zSig, sizeof(zSig) - 1, TOPIC_BATCH_SUBMITTED);
        keccak256((const uint8_t *)zCommit, sizeof(zCommit) - 1, TOPIC_BATCH_COMMITTED);
    }
    abi_function_selector("commitRange(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes)", SEL_COMMIT_RANGE);
    abi_function_selector("lastBlock()", SEL_LAST_BLOCK);
    {
        static const char z1[] = "BatchSubmitted(uint256,bytes32,bytes,uint256)";
        static const char z2[] = "BatchCommitted(uint256,bytes32,bytes,bytes,uint256)";
        static const char z3[] = "RangeCommitted(uint256,uint256,bytes32,bytes32,bytes32,bytes,uint256)";
        keccak256((const uint8_t *)z1, sizeof(z1) - 1, TOPIC3_BATCH_SUBMITTED);
        keccak256((const uint8_t *)z2, sizeof(z2) - 1, TOPIC3_BATCH_COMMITTED);
        keccak256((const uint8_t *)z3, sizeof(z3) - 1, TOPIC3_RANGE_COMMITTED);
    }

    /* HPPLiteFactory selectors */
    abi_function_selector("getRollup(address)", SEL_GET_ROLLUP);
    abi_function_selector("hasRollup(address)", SEL_HAS_ROLLUP);
    abi_function_selector("getOrCreateRollup()", SEL_GET_OR_CREATE_ROLLUP);
    abi_function_selector("getRollupsByOwner(address)", SEL_GET_ROLLUPS_BY_OWNER);
    abi_function_selector("createRollup(uint256)", SEL_CREATE_ROLLUP);
    abi_function_selector("createSimpleRollup(address)", SEL_CREATE_SIMPLE_ROLLUP);

    selectors_initialized = 1;
}

static HppliteL1 *eth_connect(const char *endpoint, const char *contractAddress) {
    if (!endpoint || !contractAddress) return NULL;

    init_selectors();

    HppliteL1 *l1 = calloc(1, sizeof(EthL1));
    if (!l1) return NULL;
    self->base.ops = &hppliteL1EthOps;

    /* Parse contract address */
    if (eth_hex_decode(contractAddress, self->contract, 20) != 0) {
        free(l1);
        return NULL;
    }

    self->chainId = hpplite_l1_default_chain_id();
    self->rpcUrl = strdup(endpoint);

    self->eth = eth_client_create(endpoint, self->chainId);
    if (!self->eth) {
        free(self->rpcUrl);
        free(l1);
        return NULL;
    }

    return l1;
}

static HppliteL1 *eth_connect_config(const HppliteNetworkConfig *config) {
    if (!config || !config->rpcUrl) return NULL;

    init_selectors();

    HppliteL1 *l1 = calloc(1, sizeof(EthL1));
    if (!l1) return NULL;
    self->base.ops = &hppliteL1EthOps;

    /* Copy contract address */
    memcpy(self->contract, config->contract, 20);

    /* Store network info */
    self->chainId = config->chainId;
    self->rpcUrl = strdup(config->rpcUrl);

    self->eth = eth_client_create(config->rpcUrl, config->chainId);
    if (!self->eth) {
        free(self->rpcUrl);
        free(l1);
        return NULL;
    }

    return l1;
}

static void eth_disconnect(HppliteL1 *l1) {
    if (!l1) return;
    free(self->aBlock);
    free(self->aRangeFrom);
    if (self->cached_state) hpplite_l1_state_free(self->cached_state);
    if (self->cached_system_config) hpplite_l1_system_config_free(self->cached_system_config);
    if (self->rpcUrl) free(self->rpcUrl);
    if (self->eth) eth_client_destroy(self->eth);
    free(l1);
}



/*
** Transaction hashes cross the L1 interface as strings the caller releases
** with sqlite3_free(): the mock builds them with sqlite3_mprintf(), so the
** real backend must too (free() on them aborts under glibc).
*/
static char *txHashString(uint8_t *tx_hash) {
    char *hex = eth_hex_encode(tx_hash, 32);
    char *out = hex ? sqlite3_mprintf("%s", hex) : NULL;
    free(hex);
    free(tx_hash);
    return out;
}

static char *eth_set_checkpoint_interval(HppliteL1 *l1, uint64_t interval) {
    uint8_t calldata[4 + 32];
    uint8_t *tx_hash;
    if (!l1 || interval == 0) return NULL;
    init_selectors();
    memcpy(calldata, SEL_SET_CHECKPOINT_INTERVAL, 4);
    abi_encode_uint64(interval, calldata + 4);
    tx_hash = eth_client_send_tx(self->eth, self->contract, calldata, sizeof(calldata), 60000, 0);
    if (!tx_hash) return NULL;
    return txHashString(tx_hash);
}


/* Set private key for signing transactions */
static char *eth_set_sequencer(HppliteL1 *l1, const unsigned char wallet[20]) {
    uint8_t calldata[4 + 32];
    uint8_t *tx_hash;
    if (!l1 || !wallet) return NULL;
    init_selectors();
    memcpy(calldata, SEL_SET_SEQUENCER, 4);
    memset(calldata + 4, 0, 32);
    memcpy(calldata + 4 + 12, wallet, 20);
    tx_hash = eth_client_send_tx(self->eth, self->contract, calldata, sizeof(calldata), 80000, 0);
    if (!tx_hash) return NULL;
    return txHashString(tx_hash);
}

static int eth_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]) {
    memcpy(self->privkey, privkey, 32);
    self->has_privkey = 1;
    eth_address_from_privkey(privkey, self->address);
    return eth_client_set_privkey(self->eth, privkey);
}

static int eth_set_signer(HppliteL1 *l1, HppliteSignFn fn, void *arg, const unsigned char address[20]) {
    if (!l1 || !fn || !address) return -1;
    memset(self->privkey, 0, 32);
    memcpy(self->address, address, 20);
    self->has_privkey = 1;
    return eth_client_set_signer(self->eth, (EthSignFn)fn, arg, address);
}

static HppliteL1State *eth_get_state(HppliteL1 *l1) {
    if (!l1) return NULL;

    /* Call getState() on contract */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_STATE, 4);

    size_t result_len;
    uint8_t *result = eth_client_call(self->eth, self->contract, calldata, 4, &result_len);
    if (!result) return NULL;

    /* Parse result: (owner, sequencerWallet, sequencerInstance, leaseExpiry, lastCpHeight, lastStateRoot, lastCpTime) */
    if (result_len < 7 * 32) {
        free(result);
        return NULL;
    }

    HppliteL1State *state = sqlite3_malloc(sizeof(HppliteL1State));   /* freed by l1.c with sqlite3_free */
    if (!state) {
        free(result);
        return NULL;
    }
    memset(state, 0, sizeof(*state));

    /* Parse each field (32 bytes each) */
    uint8_t *p = result;

    /* owner */
    abi_decode_address(p, state->owner);
    p += 32;

    /* sequencerWallet */
    abi_decode_address(p, state->sequencerWallet);
    p += 32;

    /* sequencerInstance (bytes32) */
    abi_decode_bytes32(p, state->sequencerInstance);
    p += 32;

    /* leaseExpiry */
    state->leaseExpiry = abi_decode_uint64(p);
    p += 32;

    /* lastCheckpointHeight */
    state->lastCheckpointHeight = abi_decode_uint64(p);
    p += 32;

    /* lastStateRoot */
    abi_decode_bytes32(p, state->lastStateRoot);
    p += 32;

    /* lastCheckpointTime */
    state->lastCheckpointTime = abi_decode_uint64(p);

    /* The chain's own clock, for lease arithmetic (never wall-clock time) */
    if (eth_client_latest_block(self->eth, &state->currentBlock, &state->currentTimestamp) != 0) {
        state->currentBlock = 0;
        state->currentTimestamp = (uint64_t)time(NULL);   /* callers never see 0; wall clock is the fallback */
    }

    free(result);
    return state;
}


static HppliteSystemConfig *eth_get_system_config(HppliteL1 *l1) {
    if (!l1) return NULL;

    /* Call getSystemConfig() on contract */
    /* Returns: (string daScheme, address daContract, uint256 version, uint256 chainId,
    **           uint256 checkpointInterval, uint256 leaseDuration) */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_SYSTEM_CONFIG, 4);

    size_t result_len;
    uint8_t *result = eth_client_call(self->eth, self->contract, calldata, 4, &result_len);
    if (!result) return NULL;

    /* Dynamic string + 3 values
     * Layout:
     * [0-31]   offset to string data
     * [32-63]  daContract (address)
     * [64-95]  version
     * [96-127] chainId
     * [offset] string length
     * [offset+32] string data
     */
    if (result_len < 128) {
        free(result);
        return NULL;
    }

    HppliteSystemConfig *config = sqlite3_malloc(sizeof(HppliteSystemConfig));   /* freed by l1.c with sqlite3_free */
    if (!config) {
        free(result);
        return NULL;
    }
    memset(config, 0, sizeof(*config));

    /* Parse string offset and decode string */
    uint64_t strOffset = abi_decode_uint64(result);
    if (strOffset + 32 <= result_len && strOffset + 32 >= strOffset) {         /* length word in bounds */
        uint64_t strLen = abi_decode_uint64(result + strOffset);
        if (strLen > 0 && strLen <= result_len && strOffset + 32 + strLen <= result_len) {
            config->daScheme = sqlite3_malloc((int)strLen + 1);
            if (config->daScheme) {
                memcpy(config->daScheme, result + strOffset + 32, strLen);
                config->daScheme[strLen] = '\0';
            }
        }
    }

    /* Parse address (bytes 32-63, last 20 bytes) */
    abi_decode_address(result + 32, config->daContract);

    /* Parse remaining uint256 values */
    config->version = abi_decode_uint64(result + 64);
    self->version = config->version;
    config->chainId = abi_decode_uint64(result + 96);
    /* words 4 and 5: the checkpoint interval (the configuration of record,
    ** #44) and the lease length; older contracts return four words */
    if (getenv("HPPLITE_TRACE_RPC")) fprintf(stderr, "hpplite: getSystemConfig returned %zu bytes\n", result_len);
    if (result_len >= 192) {
        config->checkpointInterval = abi_decode_uint64(result + 128);
        config->leaseDuration = abi_decode_uint64(result + 160);
    }
    config->batchSizeLimit = 128 * 1024;  /* Default, DA contract has actual limit */

    /* Cache DA contract address for batch operations */
    int is_zero = 1;
    for (int i = 0; i < 20; i++) {
        if (config->daContract[i] != 0) {
            is_zero = 0;
            break;
        }
    }
    if (!is_zero) {
        memcpy(self->da_contract, config->daContract, 20);
        self->has_da_contract = 1;
    }

    free(result);
    return config;
}

static uint64_t eth_get_lease_time_remaining(HppliteL1 *l1) {
    HppliteL1State *st = eth_get_state(l1);
    uint64_t left = 0;
    if (!st) return 0;
    if (st->leaseExpiry > st->currentTimestamp) left = st->leaseExpiry - st->currentTimestamp;
    hpplite_l1_state_free(st);
    return left;
}

static char *eth_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const unsigned char instanceId[32]
) {
    if (!l1 || !self->has_privkey || !checkpoint || !instanceId) return NULL;

    /* Build calldata for submitCheckpoint(bytes32,uint256,uint256,bytes32) */
    size_t calldata_len = 4 + 32 + 32 + 32 + 32;

    uint8_t *calldata = calloc(1, calldata_len);
    if (!calldata) return NULL;

    uint8_t *p = calldata;

    /* Function selector */
    memcpy(p, SEL_SUBMIT_CHECKPOINT, 4);
    p += 4;

    /* instanceId */
    memcpy(p, instanceId, 32);
    p += 32;

    /* fromHeight */
    abi_encode_uint64(checkpoint->fromHeight, p);
    p += 32;

    /* toHeight */
    abi_encode_uint64(checkpoint->toHeight, p);
    p += 32;

    /* stateRoot */
    abi_encode_bytes32(checkpoint->postStateRoot, p);

    /* Send transaction */
    uint8_t *tx_hash = eth_client_send_tx(self->eth, self->contract, calldata, calldata_len,
                                           500000, 0);
    free(calldata);

    if (!tx_hash) return NULL;

    char *hash_hex = txHashString(tx_hash);
    return hash_hex;
}

static char *eth_claim_sequencer(HppliteL1 *l1, const unsigned char instanceId[32]) {
    if (!l1 || !self->has_privkey || !instanceId) return NULL;
    init_selectors();

    /* Build calldata: claimSequencer(bytes32) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_CLAIM_SEQUENCER, 4);
    memcpy(calldata + 4, instanceId, 32);

    /* Send transaction */
    uint8_t *tx_hash = eth_client_send_tx(self->eth, self->contract, calldata, sizeof(calldata), 200000, 0);
    if (!tx_hash) return NULL;

    char *hash_hex = txHashString(tx_hash);
    return hash_hex;
}

static char *eth_renew_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    if (!l1 || !self->has_privkey || !instanceId) return NULL;
    init_selectors();

    /* Build calldata: renewLease(bytes32) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_RENEW_LEASE, 4);
    memcpy(calldata + 4, instanceId, 32);

    /* Send transaction */
    uint8_t *tx_hash = eth_client_send_tx(self->eth, self->contract, calldata, sizeof(calldata), 100000, 0);
    if (!tx_hash) return NULL;

    char *hash_hex = txHashString(tx_hash);
    return hash_hex;
}

static char *eth_release_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    if (!l1 || !self->has_privkey) return NULL;
    init_selectors();

    /* releaseLease(instanceId): the active instance gives the lease up (VERSION >= 5).
    ** Rollups created by the older factory only have the owner-only
    ** forceExpireLease(); the version is read on connect. */
    uint8_t *tx_hash;
    if (self->version == 0) {
        /* connected without reading the system config: ask before choosing the call */
        HppliteSystemConfig *cfg = eth_get_system_config(l1);
        if (cfg) hpplite_l1_system_config_free(cfg);
    }
    if (self->version >= 5 && instanceId) {
        uint8_t calldata[36];
        memcpy(calldata, SEL_RELEASE_LEASE, 4);
        memcpy(calldata + 4, instanceId, 32);
        tx_hash = eth_client_send_tx(self->eth, self->contract, calldata, sizeof(calldata), 60000, 0);
    } else {
        uint8_t calldata[4];
        memcpy(calldata, SEL_FORCE_EXPIRE_LEASE, 4);
        tx_hash = eth_client_send_tx(self->eth, self->contract, calldata, sizeof(calldata), 60000, 0);
    }
    if (!tx_hash) {
        fprintf(stderr, "hpplite: lease release: %s\n", eth_client_error(self->eth));
        return NULL;
    }

    char *hash_hex = txHashString(tx_hash);
    return hash_hex;
}

static int eth_get_addresses(HppliteL1 *l1, char contractHex[43], char daHex[43]) {
    static const char *h = "0123456789abcdef";
    int i;
    if (!l1) return -1;
    contractHex[0] = daHex[0] = '0'; contractHex[1] = daHex[1] = 'x';
    for (i = 0; i < 20; i++) {
        contractHex[2 + 2*i] = h[self->contract[i] >> 4]; contractHex[3 + 2*i] = h[self->contract[i] & 15];
        daHex[2 + 2*i] = h[self->da_contract[i] >> 4];     daHex[3 + 2*i] = h[self->da_contract[i] & 15];
    }
    contractHex[42] = daHex[42] = 0;
    if (!self->has_da_contract) daHex[0] = 0;
    return 0;
}

static int eth_get_my_address(HppliteL1 *l1, unsigned char out[20]) {
    if (!l1 || !self->has_privkey) return -1;
    memcpy(out, self->address, 20);
    return 0;
}

static int eth_is_lease_active(HppliteL1 *l1) {
    if (!l1) return 0;

    uint8_t calldata[4];
    memcpy(calldata, SEL_IS_LEASE_ACTIVE, 4);
    
    size_t result_len;
    uint8_t *result = eth_client_call(self->eth, self->contract, calldata, 4, &result_len);
    if (!result || result_len < 32) {
        free(result);
        return 0;
    }
    
    int expired = (abi_decode_uint64(result) != 0);
    free(result);
    return expired;
}

static void eth_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg) {
    if (!l1) return;
    self->callback = callback;
    self->callback_arg = arg;
}

static int eth_poll(HppliteL1 *l1) {
    /* For now, just refresh state - no event monitoring yet */
    if (!l1) return -1;
    
    HppliteL1State *new_state = eth_get_state(l1);
    if (!new_state) return -1;
    
    /* Check for changes and fire callbacks */
    if (self->cached_state && self->callback) {
        /* Check for new checkpoint */
        if (new_state->lastCheckpointHeight > self->cached_state->lastCheckpointHeight) {
            self->callback(self->callback_arg, "checkpoint", new_state);
        }
    }
    
    if (self->cached_state) hpplite_l1_state_free(self->cached_state);
    self->cached_state = new_state;

    return 0;
}

static int eth_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs) {
    if (!l1 || !tx_hash) return -1;

    /* Parse tx hash */
    uint8_t hash_bytes[32];
    if (eth_hex_decode(tx_hash, hash_bytes, 32) != 0) {
        return -1;
    }

    /* Poll for receipt */
    for (int i = 0; i < timeout_secs; i++) {
        int status = 0;
        int result = eth_client_get_receipt(self->eth, hash_bytes, &status);

        if (result == 1) {
            /* Mined */
            return status;  /* 1 = success, 0 = reverted */
        }
        if (result < 0) {
            return -1;  /* Error */
        }

        /* Still pending, wait */
        sleep(1);
    }

    return -1;  /* Timeout */
}

/* ============ Factory Functions ============ */

static int eth_factory_get_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t rollup_address_out[20]
) {
    init_selectors();

    /* Parse factory address */
    uint8_t factory[20];
    const char *factory_addr = factory_address ? factory_address : DEFAULT_FACTORY_ADDRESS;
    if (eth_hex_decode(factory_addr, factory, 20) != 0) {
        return -1;
    }

    /* Create temporary eth client */
    EthClient *eth = eth_client_create(rpc_url, hpplite_l1_default_chain_id());
    if (!eth) return -1;

    /* Build calldata: getRollup(address) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_GET_ROLLUP, 4);
    abi_encode_address(owner_address, calldata + 4);

    size_t result_len;
    uint8_t *result = eth_client_call(eth, factory, calldata, sizeof(calldata), &result_len);
    eth_client_destroy(eth);

    if (!result || result_len < 32) {
        free(result);
        return -1;
    }

    /* Decode address from result */
    abi_decode_address(result, rollup_address_out);
    free(result);

    /* Check if zero address (no rollup) */
    int is_zero = 1;
    for (int i = 0; i < 20; i++) {
        if (rollup_address_out[i] != 0) {
            is_zero = 0;
            break;
        }
    }

    return is_zero ? 0 : 1;  /* 0 = no rollup, 1 = has rollup */
}

static int eth_factory_has_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20]
) {
    uint8_t rollup[20];
    return eth_factory_get_rollup(rpc_url, factory_address, owner_address, rollup);
}

static char *eth_factory_get_or_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    init_selectors();

    /* Parse factory address */
    uint8_t factory[20];
    const char *factory_addr = factory_address ? factory_address : DEFAULT_FACTORY_ADDRESS;
    if (eth_hex_decode(factory_addr, factory, 20) != 0) {
        return NULL;
    }

    /* Create eth client with private key */
    EthClient *eth = eth_client_create(rpc_url, hpplite_l1_default_chain_id());
    if (!eth) return NULL;

    if (eth_client_set_privkey(eth, privkey) != 0) {
        eth_client_destroy(eth);
        return NULL;
    }

    /* Build calldata: getOrCreateRollup() - no parameters */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_OR_CREATE_ROLLUP, 4);

    /* Send transaction - needs high gas for deploying child HPPLiteDA contract */
    uint8_t *tx_hash = eth_client_send_tx(eth, factory, calldata, 4, 15000000, 0);

    if (!tx_hash) {
        eth_client_destroy(eth);
        return NULL;
    }
    eth_client_destroy(eth);

    char *hash_hex = txHashString(tx_hash);

    /* Caller should wait for tx and then call get_rollup to get the address */
    return hash_hex;
}

/* Every rollup the owner created, oldest first (#61). Returns how many were
** written to out (at most max), or -1 */
static int eth_factory_list_rollups(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t (*out)[20],
    int max
) {
    uint8_t factory[20], calldata[4 + 32];
    const char *factory_addr = factory_address ? factory_address : DEFAULT_FACTORY_ADDRESS;
    size_t len = 0;
    uint8_t *result;
    uint64_t n = 0;
    int i, got = 0;
    EthClient *eth;
    init_selectors();
    if (eth_hex_decode(factory_addr, factory, 20) != 0) return -1;
    eth = eth_client_create(rpc_url, hpplite_l1_default_chain_id());
    if (!eth) return -1;
    memcpy(calldata, SEL_GET_ROLLUPS_BY_OWNER, 4);
    abi_encode_address(owner_address, calldata + 4);
    result = eth_client_call(eth, factory, calldata, sizeof(calldata), &len);
    eth_client_destroy(eth);
    /* address[]: offset, length, then one word per address */
    if (!result || len < 64) { free(result); return -1; }
    for (i = 56; i < 64; i++) n = n << 8 | result[i];
    if (len < 64 + n * 32) { free(result); return -1; }
    for (i = 0; i < (int)n && got < max; i++) abi_decode_address(result + 64 + 32 * i, out[got++]);
    free(result);
    return got;
}

/* A new rollup for the wallet, whatever it has already (#61): waits for the
** transaction and takes the address from its RollupCreated event - not the
** last one listed, which another process of the wallet may have created at
** the same moment. 1 created (rollup_out set), 0 failed */
static int eth_factory_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint64_t checkpointInterval,
    uint8_t rollup_out[20]
) {
    uint8_t factory[20], calldata[4 + 32], topic0[32], word[32], *tx_hash;
    const char *factory_addr = factory_address ? factory_address : DEFAULT_FACTORY_ADDRESS;
    const char *sig = "RollupCreated(address,address,address,uint256)";
    EthClient *eth;
    int i, rc = 0;
    init_selectors();
    if (eth_hex_decode(factory_addr, factory, 20) != 0) return 0;
    eth = eth_client_create(rpc_url, hpplite_l1_default_chain_id());
    if (!eth) return 0;
    if (eth_client_set_privkey(eth, privkey) != 0) { eth_client_destroy(eth); return 0; }
    memcpy(calldata, SEL_CREATE_ROLLUP, 4);
    abi_encode_uint64(checkpointInterval, calldata + 4);
    /* deploys the rollup and its DA contract: the gas getOrCreateRollup() takes */
    tx_hash = eth_client_send_tx(eth, factory, calldata, sizeof(calldata), 15000000, 0);
    if (!tx_hash) { eth_client_destroy(eth); return 0; }
    keccak256((const uint8_t *)sig, strlen(sig), topic0);
    for (i = 0; i < 240 && (rc == 0 || rc == -2); i++) {   /* up to a minute; an RPC error is not an answer (#68) */
        rc = eth_client_receipt_topic(eth, tx_hash, topic0, 1, word);
        if (rc != 1 && rc != -1) usleep(250000);
    }
    if (rc != 1) {
        /* the transaction may still land: the wallet would then own a
        ** rollup no database is bound to, and the next open creates another */
        char *zHash = eth_hex_encode(tx_hash, 32);
        fprintf(stderr, "hpplite: createRollup transaction %s %s; if it lands, this wallet gets a rollup no database"
                " is bound to\n", zHash ? zHash : "?", rc == -1 ? "reverted or raised no RollupCreated event" : "not confirmed within a minute");
        free(zHash);
    }
    free(tx_hash);
    eth_client_destroy(eth);
    if (rc != 1) return 0;
    memcpy(rollup_out, word + 12, 20);              /* an indexed address: the topic's low 20 bytes */
    return 1;
}

/* The same lists, for the wallet of a private key */
static int eth_factory_list_my_rollups(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t (*out)[20],
    int max
) {
    uint8_t me[20];
    if (eth_address_from_privkey(privkey, me) != 0) return -1;
    return eth_factory_list_rollups(rpc_url, factory_address, me, out, max);
}

/* Helper: Get rollup address from private key (derive address, then lookup) */
static int eth_factory_get_my_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    /* Derive address from private key */
    uint8_t my_address[20];
    if (eth_address_from_privkey(privkey, my_address) != 0) {
        return -1;
    }

    return eth_factory_get_rollup(rpc_url, factory_address, my_address, rollup_address_out);
}

/* Connect to L1 using factory - auto-discovers rollup for this wallet */
static HppliteL1 *eth_connect_factory(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32]
) {
    /* Get rollup address for this wallet */
    uint8_t rollup[20];
    int result = eth_factory_get_my_rollup(rpc_url, factory_address, privkey, rollup);

    if (result != 1) {
        /* No rollup exists for this wallet */
        return NULL;
    }

    /* Convert rollup address to hex */
    char *rollup_hex = eth_hex_encode(rollup, 20);
    if (!rollup_hex) return NULL;

    /* Connect to the rollup contract */
    HppliteL1 *l1 = eth_connect(rpc_url, rollup_hex);
    free(rollup_hex);

    if (l1) {
        /* Set private key for transactions */
        eth_set_privkey(l1, privkey);
    }

    return l1;
}

/* ============ Batch DA Functions ============ */

/* Helper to ensure DA contract is cached */
static int ensure_da_contract(HppliteL1 *l1) {
    if (self->has_da_contract) return 1;

    /* Try to get system config which caches DA contract */
    HppliteSystemConfig *config = eth_get_system_config(l1);
    if (config) {
        hpplite_l1_system_config_free(config);
    }

    return self->has_da_contract;
}

static char *eth_submit_batch(
    HppliteL1 *l1,
    const uint8_t instanceId[32],
    uint64_t height,
    const uint8_t *data,
    size_t data_len
) {
    if (l1) self->lastOversized = 0;
    if (!l1 || !self->has_privkey || !instanceId || !data || data_len == 0) return NULL;
    if (!ensure_da_contract(l1)) return NULL;  /* Need DA contract */

    /* Build calldata: submitBatch(bytes32 instanceId, uint256 height, bytes data) */
    /* Static: 4 + 32 + 32 + 32 (offset) = 100 */
    /* Dynamic: 32 (length) + padded data */
    size_t padded_len = ((data_len + 31) / 32) * 32;
    size_t calldata_len = 4 + 32 + 32 + 32 + 32 + padded_len;

    uint8_t *calldata = calloc(1, calldata_len);
    if (!calldata) return NULL;

    uint8_t *p = calldata;

    /* Function selector */
    memcpy(p, SEL_SUBMIT_BATCH, 4);
    p += 4;

    /* instanceId (bytes32) */
    memcpy(p, instanceId, 32);
    p += 32;

    /* height */
    abi_encode_uint64(height, p);
    p += 32;

    /* bytes offset (96 from start of params = 3 * 32) */
    abi_encode_uint64(96, p);
    p += 32;

    /* bytes length */
    abi_encode_uint64(data_len, p);
    p += 32;

    /* bytes data */
    memcpy(p, data, data_len);

    /* Send transaction to DA contract
     * Gas calculation:
     * - Base tx cost: 21000
     * - Per non-zero byte: 16 (calldata can be large for batch JSON)
     * - Contract storage: ~20000 per 32-byte slot (expensive!)
     * - On L2s like HPP: additional L1 data posting costs
     * For batch storage, we need significant gas for SSTORE operations.
     */
    /* Ask the chain: storage costs differ per chain and a fixed per-byte
    ** formula sat right at the edge (fresh storage is ~707 gas/byte; the
    ** old 700 ran out of gas on larger batches). 25% margin; the formula
    ** is only the fallback when the estimate call fails. */
    uint64_t gas_estimate = eth_client_estimate_gas(self->eth, self->da_contract, calldata, calldata_len);
    if (gas_estimate != (uint64_t)-1 && self->maxTxGas && gas_estimate > self->maxTxGas) {
        /* more than a transaction may use (HPP Sepolia: 32M): sent, it would
        ** run out of gas and revert with all of it spent - do not send */
        free(calldata);
        self->lastOversized = 1;
        return NULL;
    }
    if (gas_estimate == (uint64_t)-1) gas_estimate = 300000 + data_len * 1000;
    else gas_estimate += gas_estimate / 4;
    if (gas_estimate < 3000000) gas_estimate = 3000000;
    if (self->maxTxGas && gas_estimate > self->maxTxGas) gas_estimate = self->maxTxGas;
    uint8_t *tx_hash = eth_client_send_tx(self->eth, self->da_contract, calldata, calldata_len,
                                           gas_estimate, 0);
    free(calldata);

    if (!tx_hash) {
        /* geth's txpool answers "oversized data": never sent, will never be */
        const char *e = eth_client_error(self->eth);
        if (e && strstr(e, "oversized")) self->lastOversized = 1;
        return NULL;
    }

    char *hash_hex = txHashString(tx_hash);
    return hash_hex;
}

/* A word of ABI-encoded data, 0 when out of range */
static uint64_t abi_word_u64(const uint8_t *data, size_t len, int i) {
    return (len >= (size_t)(32 * (i + 1))) ? abi_decode_uint64(data + 32 * i) : 0;
}

/*
** DA v3 (#164): the contract keeps no block per height; every event names
** the block of the commitment before it. The index walks the events
** backward from lastBlock() to the block the previous walk started from,
** one eth_getLogs per block, and records per height its block and, when the
** height is inside an anchored range, the range's first height.
*/
static int eth_index_update(HppliteL1 *l1) {
    uint64_t last = 0, total = 0, block, top, stop;
    uint8_t calldata[4], latest[32];
    uint8_t *result;
    size_t result_len = 0;
    if (eth_get_da_state(l1, &last, &total, latest) != 0) return -1;
    if (last <= self->indexedHeight) return 0;
    if (last + 1 > self->nIndexAlloc) {
        uint64_t n = self->nIndexAlloc ? self->nIndexAlloc : 1024;
        uint64_t *b, *r;
        while (n < last + 1) n *= 2;
        b = realloc(self->aBlock, (size_t)n * sizeof(uint64_t));
        if (!b) return -1;
        self->aBlock = b;
        r = realloc(self->aRangeFrom, (size_t)n * sizeof(uint64_t));
        if (!r) return -1;
        self->aRangeFrom = r;
        memset(self->aBlock + self->nIndexAlloc, 0, (size_t)(n - self->nIndexAlloc) * sizeof(uint64_t));
        memset(self->aRangeFrom + self->nIndexAlloc, 0, (size_t)(n - self->nIndexAlloc) * sizeof(uint64_t));
        self->nIndexAlloc = n;
    }
    memcpy(calldata, SEL_LAST_BLOCK, 4);
    result = eth_client_call(self->eth, self->da_contract, calldata, 4, &result_len);
    if (!result || result_len < 32) { free(result); return -1; }
    top = block = abi_decode_uint64(result);
    free(result);
    stop = self->lastIndexedBlock;
    while (block > stop) {
        EthLog *logs = NULL;
        int n = 0, i;
        uint64_t prev = block;
        if (eth_client_get_block_logs(self->eth, self->da_contract, block, &logs, &n) != 0) return -1;
        for (i = 0; i < n; i++) {
            EthLog *e = &logs[i];
            uint64_t p = block, h, from, to;
            if (e->nTopics < 2) continue;
            if (memcmp(e->topics[0], TOPIC3_BATCH_SUBMITTED, 32) == 0 || memcmp(e->topics[0], TOPIC3_BATCH_COMMITTED, 32) == 0) {
                h = abi_decode_uint64(e->topics[1]);
                if (h >= 1 && h <= last) { self->aBlock[h] = block; self->aRangeFrom[h] = 0; }
                p = abi_word_u64(e->data, e->nData, memcmp(e->topics[0], TOPIC3_BATCH_SUBMITTED, 32) == 0 ? 1 : 2);
            } else if (memcmp(e->topics[0], TOPIC3_RANGE_COMMITTED, 32) == 0 && e->nTopics >= 3) {
                from = abi_decode_uint64(e->topics[1]);
                to = abi_decode_uint64(e->topics[2]);
                if (to > last) to = last;
                for (h = from; h >= 1 && h <= to; h++) { self->aBlock[h] = block; self->aRangeFrom[h] = from; }
                p = abi_word_u64(e->data, e->nData, 4);
            } else {
                continue;
            }
            if (p < prev) prev = p;
        }
        eth_logs_free(logs, n);
        if (n == 0 || prev >= block) break;      /* nothing of ours here, or no earlier commitment named: the walk ends */
        block = prev;
    }
    self->lastIndexedBlock = top;
    self->indexedHeight = last;
    return 0;
}

static int eth_has_ranges(HppliteL1 *l1) {
    return l1 && ensure_da_contract(l1) && self->version >= 9;
}

/* DA v3 commitRange (#164): one transaction for every batch stored since the last commitment */
static char *eth_commit_range(HppliteL1 *l1, const uint8_t instanceId[32], const HppliteRange *r) {
    size_t rcptLen, rcptPad, n;
    uint8_t *calldata, *p, *tx_hash;
    uint64_t gas;
    if (!l1 || !self->has_privkey || !instanceId || !r) return NULL;
    if (!ensure_da_contract(l1) || self->version < 9) return NULL;
    rcptLen = r->receipt ? r->nReceipt : 0;
    rcptPad = (rcptLen + 31) / 32 * 32;
    n = 4 + 7 * 32 + 32 + rcptPad;
    calldata = calloc(1, n);
    if (!calldata) return NULL;
    p = calldata;
    memcpy(p, SEL_COMMIT_RANGE, 4); p += 4;
    memcpy(p, instanceId, 32); p += 32;
    abi_encode_uint64(r->fromHeight, p); p += 32;
    abi_encode_uint64(r->toHeight, p); p += 32;
    memcpy(p, r->chainHashAfter, 32); p += 32;
    memcpy(p, r->stateRoot, 32); p += 32;
    memcpy(p, r->manifestDigest, 32); p += 32;
    abi_encode_uint64(7 * 32, p); p += 32;                         /* the receipt's offset from the start of the arguments */
    abi_encode_uint64(rcptLen, p); p += 32;
    if (rcptLen) memcpy(p, r->receipt, rcptLen);
    gas = eth_client_estimate_gas(self->eth, self->da_contract, calldata, n);
    if (gas == (uint64_t)-1) gas = 300000 + rcptLen * 100;
    else gas += gas / 4;
    if (gas < 200000) gas = 200000;
    if (self->maxTxGas && gas > self->maxTxGas) gas = self->maxTxGas;
    tx_hash = eth_client_send_tx(self->eth, self->da_contract, calldata, n, gas, 0);
    free(calldata);
    return tx_hash ? txHashString(tx_hash) : NULL;
}

static uint8_t *abi_dynamic_bytes(const uint8_t *raw, size_t raw_len, int i, size_t *pLen);

/* RangeCommitted(from, to, chainHashAfter, stateRoot, manifestDigest, bytes receipt, prevBlock) for the range `height` is inside */
static int eth_get_range(HppliteL1 *l1, uint64_t height, HppliteRange *out) {
    uint8_t topic1[32], topic2[32], *raw, *rcpt;
    size_t raw_len = 0, rcptLen = 0;
    uint64_t from;
    if (!l1 || !out || !ensure_da_contract(l1) || self->version < 9) return -1;
    if (eth_index_update(l1) != 0 || height == 0 || height > self->indexedHeight) return -1;
    from = self->aRangeFrom[height];
    if (from == 0) return -1;
    abi_encode_uint64(from, topic1);
    raw = eth_client_get_log(self->eth, self->da_contract, self->aBlock[height], TOPIC3_RANGE_COMMITTED, topic1, &raw_len, topic2);
    if (!raw || raw_len < 5 * 32) { free(raw); return -1; }
    out->fromHeight = from;
    out->toHeight = abi_decode_uint64(topic2);
    memcpy(out->chainHashAfter, raw, 32);
    memcpy(out->stateRoot, raw + 32, 32);
    memcpy(out->manifestDigest, raw + 64, 32);
    rcpt = abi_dynamic_bytes(raw, raw_len, 3, &rcptLen);
    free(raw);
    out->receipt = NULL; out->nReceipt = 0;
    if (rcpt && rcptLen) {
        out->receipt = sqlite3_malloc((int)rcptLen);
        if (out->receipt) { memcpy(out->receipt, rcpt, rcptLen); out->nReceipt = rcptLen; }
    }
    free(rcpt);
    return 0;
}

/*
** DA v2 (#33): the batch's bytes are in the log of the block blockOf(height)
** names, as the ABI-encoded `bytes data` of BatchSubmitted; its hash is the
** log's third topic. A height recorded by commitBatch (bytes elsewhere, #11)
** has no such log: NULL, as a missing batch.
*/
/* The ABI-encoded data of the log at `height` whose signature is `topic0`, and its hash topic */
static uint8_t *eth_height_log(HppliteL1 *l1, uint64_t height, const uint8_t topic0[32], size_t *raw_len, uint8_t hash_out[32]) {
    uint8_t calldata[4 + 32], topic1[32];
    size_t result_len = 0;
    uint64_t block;
    uint8_t *result;

    if (self->version >= 9) {
        /* DA v3: no blockOf(); the index, walked from the events (#164) */
        if (eth_index_update(l1) != 0 || height == 0 || height > self->indexedHeight || self->aBlock[height] == 0) return NULL;
        if (self->aRangeFrom[height]) return NULL;        /* inside a range: no log of its own (eth_get_range) */
        block = self->aBlock[height];
        if (topic0 == TOPIC_BATCH_SUBMITTED) topic0 = TOPIC3_BATCH_SUBMITTED;
        else if (topic0 == TOPIC_BATCH_COMMITTED) topic0 = TOPIC3_BATCH_COMMITTED;
    } else {
        memcpy(calldata, SEL_BLOCK_OF, 4);
        abi_encode_uint64(height, calldata + 4);
        result = eth_client_call(self->eth, self->da_contract, calldata, sizeof(calldata), &result_len);
        if (!result || result_len < 32) { free(result); return NULL; }   /* "No such batch" reverts */
        block = abi_decode_uint64(result);
        free(result);
    }
    abi_encode_uint64(height, topic1);
    return eth_client_get_log(self->eth, self->da_contract, block, topic0, topic1, raw_len, hash_out);
}

/* The i-th dynamic `bytes` of an ABI tuple: malloc'd, terminated; NULL when malformed */
static uint8_t *abi_dynamic_bytes(const uint8_t *raw, size_t raw_len, int i, size_t *pLen) {
    uint64_t offset, len;
    uint8_t *data;
    if (raw_len < (size_t)(32 * (i + 1))) return NULL;
    offset = abi_decode_uint64(raw + 32 * i);
    if (offset + 32 > raw_len || offset + 32 < offset) return NULL;
    len = abi_decode_uint64(raw + offset);
    if (len > raw_len - offset - 32) return NULL;
    data = malloc((size_t)len + 1);
    if (!data) return NULL;
    memcpy(data, raw + offset + 32, (size_t)len);
    data[len] = 0;
    if (pLen) *pLen = (size_t)len;
    return data;
}

static uint8_t *eth_get_batch_from_log(HppliteL1 *l1, uint64_t height, size_t *data_len_out, uint8_t hash_out[32]) {
    size_t raw_len = 0;
    uint8_t *raw = eth_height_log(l1, height, TOPIC_BATCH_SUBMITTED, &raw_len, hash_out);
    uint8_t *data;
    if (!raw) return NULL;
    data = abi_dynamic_bytes(raw, raw_len, 0, data_len_out);
    free(raw);
    return data;
}

/* DA v2 commitBatch(bytes32 instanceId, uint256 height, bytes32 hash, bytes ref, bytes receipt) (#11) */
static char *eth_commit_batch(HppliteL1 *l1, const uint8_t instanceId[32], uint64_t height, const uint8_t hash[32],
                              const char *zRef, const uint8_t *receipt, size_t nReceipt) {
    size_t refLen, rcptLen, refPad, rcptPad, n;
    uint8_t *calldata, *p, *tx_hash;
    uint64_t gas;
    if (!l1 || !self->has_privkey || !instanceId || !hash || !zRef || !zRef[0]) return NULL;
    if (!ensure_da_contract(l1) || self->version < 8) return NULL;
    refLen = strlen(zRef);
    rcptLen = receipt ? nReceipt : 0;
    refPad = (refLen + 31) / 32 * 32;
    rcptPad = (rcptLen + 31) / 32 * 32;
    n = 4 + 5 * 32 + 32 + refPad + 32 + rcptPad;
    calldata = calloc(1, n);
    if (!calldata) return NULL;
    p = calldata;
    memcpy(p, SEL_COMMIT_BATCH, 4); p += 4;
    memcpy(p, instanceId, 32); p += 32;
    abi_encode_uint64(height, p); p += 32;
    memcpy(p, hash, 32); p += 32;
    abi_encode_uint64(5 * 32, p); p += 32;                         /* ref's offset from the start of the arguments */
    abi_encode_uint64(5 * 32 + 32 + refPad, p); p += 32;           /* receipt's */
    abi_encode_uint64(refLen, p); p += 32;
    memcpy(p, zRef, refLen); p += refPad;
    abi_encode_uint64(rcptLen, p); p += 32;
    if (rcptLen) memcpy(p, receipt, rcptLen);
    gas = eth_client_estimate_gas(self->eth, self->da_contract, calldata, n);
    if (gas == (uint64_t)-1) gas = 300000 + (refLen + rcptLen) * 100;
    else gas += gas / 4;
    if (gas < 200000) gas = 200000;
    if (self->maxTxGas && gas > self->maxTxGas) gas = self->maxTxGas;
    tx_hash = eth_client_send_tx(self->eth, self->da_contract, calldata, n, gas, 0);
    free(calldata);
    return tx_hash ? txHashString(tx_hash) : NULL;
}

/* BatchCommitted(height, hash, bytes ref, bytes receipt) at `height` (#11) */
static int eth_get_commitment(HppliteL1 *l1, uint64_t height, uint8_t hash[32], char **pzRef, uint8_t **pReceipt, size_t *pnReceipt) {
    size_t raw_len = 0, refLen = 0, rcptLen = 0;
    uint8_t h[32], *raw, *ref, *rcpt;
    if (pzRef) *pzRef = NULL;
    if (pReceipt) *pReceipt = NULL;
    if (pnReceipt) *pnReceipt = 0;
    if (!l1 || !ensure_da_contract(l1) || self->version < 8) return -1;
    raw = eth_height_log(l1, height, TOPIC_BATCH_COMMITTED, &raw_len, h);
    if (!raw) return -1;
    ref = abi_dynamic_bytes(raw, raw_len, 0, &refLen);
    rcpt = abi_dynamic_bytes(raw, raw_len, 1, &rcptLen);
    free(raw);
    if (!ref || !refLen) { free(ref); free(rcpt); return -1; }
    if (hash) memcpy(hash, h, 32);
    if (pzRef) *pzRef = sqlite3_mprintf("%.*s", (int)refLen, (const char *)ref);
    if (pReceipt && rcpt && rcptLen) {
        *pReceipt = sqlite3_malloc((int)rcptLen);
        if (*pReceipt) { memcpy(*pReceipt, rcpt, rcptLen); if (pnReceipt) *pnReceipt = rcptLen; }
    }
    free(ref);
    free(rcpt);
    return 0;
}

static uint8_t *eth_get_batch(
    HppliteL1 *l1,
    uint64_t height,
    size_t *data_len_out
) {
    if (!l1 || !data_len_out) return NULL;
    if (!ensure_da_contract(l1)) {
        *data_len_out = 0;
        return NULL;
    }
    if (self->version >= 8) {
        uint8_t *data = eth_get_batch_from_log(l1, height, data_len_out, NULL);
        if (!data) *data_len_out = 0;
        return data;
    }

    /* Build calldata: getBatch(uint256 height) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_GET_BATCH, 4);
    abi_encode_uint64(height, calldata + 4);

    size_t result_len;
    uint8_t *result = eth_client_call(self->eth, self->da_contract, calldata, sizeof(calldata), &result_len);
    if (!result || result_len < 64) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    /* Dynamic bytes: offset (32) + length (32) + data */
    uint64_t offset = abi_decode_uint64(result);
    if (offset >= result_len) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    uint64_t len = abi_decode_uint64(result + offset);
    if (offset + 32 > result_len || offset + 32 < offset) { free(result); return NULL; }   /* length word in bounds */
    if (len == 0 || len > result_len || offset + 32 + len > result_len) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    /* Copy data, terminated: a plaintext batch is parsed as a C string, and
    ** one cut short (by the chain, or by whoever posted it) must not run
    ** the parser past the end (#77) */
    uint8_t *data = malloc(len + 1);
    if (!data) {
        free(result);
        *data_len_out = 0;
        return NULL;
    }

    memcpy(data, result + offset + 32, len);
    data[len] = 0;
    *data_len_out = len;

    free(result);
    return data;
}

static int eth_get_batch_hash(
    HppliteL1 *l1,
    uint64_t height,
    uint8_t hash_out[32]
) {
    if (!l1) return -1;
    if (!ensure_da_contract(l1)) return -1;
    if (self->version >= 8) {
        size_t n = 0;
        uint8_t *data = eth_get_batch_from_log(l1, height, &n, hash_out);
        if (!data) { memset(hash_out, 0, 32); return 0; }
        free(data);
        return 1;
    }

    /* Build calldata: getBatchHash(uint256 height) */
    uint8_t calldata[4 + 32];
    memcpy(calldata, SEL_GET_BATCH_HASH, 4);
    abi_encode_uint64(height, calldata + 4);

    size_t result_len;
    uint8_t *result = eth_client_call(self->eth, self->da_contract, calldata, sizeof(calldata), &result_len);
    if (!result || result_len < 32) {
        free(result);
        return -1;
    }

    memcpy(hash_out, result, 32);
    free(result);

    /* Check if zero (no batch at this height) */
    int is_zero = 1;
    for (int i = 0; i < 32; i++) {
        if (hash_out[i] != 0) {
            is_zero = 0;
            break;
        }
    }

    return is_zero ? 0 : 1;  /* 0 = no batch, 1 = has batch */
}

static int eth_get_da_state(
    HppliteL1 *l1,
    uint64_t *last_batch_height,
    uint64_t *total_batches,
    uint8_t latest_batch_hash[32]
) {
    if (!l1) return -1;
    if (!ensure_da_contract(l1)) return -1;

    /* Build calldata: getDAState() */
    uint8_t calldata[4];
    memcpy(calldata, SEL_GET_DA_STATE, 4);

    size_t result_len;
    uint8_t *result = eth_client_call(self->eth, self->da_contract, calldata, 4, &result_len);
    if (!result || result_len < 96) {
        free(result);
        return -1;
    }

    /* Parse: (uint256 lastBatchHeight, uint256 totalBatches, bytes32 latestBatchHash) */
    if (last_batch_height) *last_batch_height = abi_decode_uint64(result);
    if (total_batches) *total_batches = abi_decode_uint64(result + 32);
    if (latest_batch_hash) memcpy(latest_batch_hash, result + 64, 32);

    free(result);
    return 0;
}

/* ============ Peer Discovery Functions ============ */

/* Endpoint functions removed in singleton model - no peer discovery needed */

static int eth_submit_was_oversized(HppliteL1 *l1) {
    return l1 ? self->lastOversized : 0;
}

/* DA v2: getDAState()'s third word is the rolling hash (#33); v1 kept the latest batch's hash there */
static int eth_get_chain_hash(HppliteL1 *l1, uint64_t *pHeight, uint8_t out[32]) {
    uint64_t last = 0, total = 0;
    if (!l1 || !ensure_da_contract(l1) || self->version < 8) return -1;
    if (eth_get_da_state(l1, &last, &total, out) != 0) return -1;
    if (pHeight) *pHeight = last;
    return 0;
}

static void eth_set_max_tx_gas(HppliteL1 *l1, uint64_t gas) {
    if (l1) self->maxTxGas = gas;
}

static uint64_t eth_chain_id(HppliteL1 *l1) {
    return l1 ? eth_client_chain_id(self->eth) : 0;
}

/* balanceOf(address) (#11): selector 70a08231, the owner left-padded to a word */
static int eth_token_balance(HppliteL1 *l1, const unsigned char token[20], const unsigned char owner[20], uint8_t out[32]) {
    uint8_t calldata[36] = { 0x70, 0xa0, 0x82, 0x31 };
    size_t result_len = 0;
    uint8_t *result;
    if (!l1 || !token || !owner || !out) return -1;
    memcpy(calldata + 4 + 12, owner, 20);
    result = eth_client_call(self->eth, token, calldata, sizeof calldata, &result_len);
    if (!result || result_len < 32) { free(result); return -1; }
    memcpy(out, result, 32);
    free(result);
    return 0;
}

const HppliteL1Ops hppliteL1EthOps = {
    .name = "eth",
    .connect = eth_connect,
    .connect_config = eth_connect_config,
    .connect_factory = eth_connect_factory,
    .disconnect = eth_disconnect,
    .get_system_config = eth_get_system_config,
    .set_checkpoint_interval = eth_set_checkpoint_interval,
    .set_privkey = eth_set_privkey,
    .set_signer = eth_set_signer,
    .set_sequencer = eth_set_sequencer,
    .get_state = eth_get_state,
    .claim_sequencer = eth_claim_sequencer,
    .renew_lease = eth_renew_lease,
    .release_lease = eth_release_lease,
    .is_lease_active = eth_is_lease_active,
    .get_addresses = eth_get_addresses,
    .get_my_address = eth_get_my_address,
    .get_lease_time_remaining = eth_get_lease_time_remaining,
    .submit_checkpoint = eth_submit_checkpoint,
    .set_callback = eth_set_callback,
    .poll = eth_poll,
    .wait_for_tx = eth_wait_for_tx,
    .factory_get_rollup = eth_factory_get_rollup,
    .factory_has_rollup = eth_factory_has_rollup,
    .factory_get_or_create_rollup = eth_factory_get_or_create_rollup,
    .factory_list_rollups = eth_factory_list_rollups,
    .factory_create_rollup = eth_factory_create_rollup,
    .factory_list_my_rollups = eth_factory_list_my_rollups,
    .factory_get_my_rollup = eth_factory_get_my_rollup,
    .submit_batch = eth_submit_batch,
    .get_batch = eth_get_batch,
    .get_batch_hash = eth_get_batch_hash,
    .get_da_state = eth_get_da_state,
    .submit_was_oversized = eth_submit_was_oversized,
    .set_max_tx_gas = eth_set_max_tx_gas,
    .get_chain_hash = eth_get_chain_hash,
    .commit_batch = eth_commit_batch,
    .get_commitment = eth_get_commitment,
    .commit_range = eth_commit_range,
    .get_range = eth_get_range,
    .has_ranges = eth_has_ranges,
    .chain_id = eth_chain_id,
    .token_balance = eth_token_balance,
};
