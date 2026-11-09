/*
** HPPLite L1 Interface
**
** Abstract interface for L1 blockchain interaction.
** Provides mock implementation for testing, can be swapped
** for real Ethereum/L1 client in production.
**
** L1 responsibilities:
** - Sequencer lease management (singleton model)
** - Checkpoint commitments
** - Batch data availability
** - Configuration
*/

#ifndef HPPLITE_L1_INTERFACE_H
#define HPPLITE_L1_INTERFACE_H

#include "batch.h"
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** Network configuration (for bootstrapping)
*/
typedef struct HppliteNetworkConfig {
    const char *alias;           /* Network alias: "hpp-sepolia" (optional) */
    uint64_t chainId;            /* Chain ID: 181228 */
    const char *rpcUrl;          /* RPC URL: "https://sepolia.hpp.io" */
    unsigned char contract[20];  /* Contract address */
} HppliteNetworkConfig;

/*
** System configuration (read from L1 contract)
*/
typedef struct HppliteSystemConfig {
    char *daScheme;              /* DA URI scheme: "hppda", "ipfs", "file" */
    unsigned char daContract[20]; /* DA contract address */
    uint64_t batchSizeLimit;     /* Max batch size in bytes */
    uint64_t version;            /* Contract version */
    uint64_t chainId;            /* Chain ID */
    uint64_t checkpointInterval; /* Batches between checkpoints */
    uint64_t leaseDuration;      /* Lease duration in seconds */
} HppliteSystemConfig;

/*
** Built-in network definitions
*/
#define HPPLITE_NETWORK_HPP_SEPOLIA { \
    .alias = "hpp-sepolia", \
    .chainId = 181228, \
    .rpcUrl = "https://sepolia.hpp.io", \
    .contract = {0} \
}

/*
** L1 connection context
*/
typedef struct HppliteL1 HppliteL1;

/*
** L1 contract state (read from chain)
*/
typedef struct HppliteL1State {
    /* Owner */
    unsigned char owner[20];

    /* Current sequencer */
    unsigned char sequencerWallet[20];
    unsigned char sequencerInstance[32];
    uint64_t leaseExpiry;

    /* Configuration */
    uint64_t checkpointInterval;
    uint64_t leaseDuration;

    /* Latest checkpoint */
    uint64_t lastCheckpointHeight;
    unsigned char lastStateRoot[32];
    uint64_t lastCheckpointTime;

    /* Current L1 state */
    uint64_t currentBlock;
    uint64_t currentTimestamp;

} HppliteL1State;

/*
** Callback for L1 events
*/
typedef void (*HppliteL1Callback)(void *arg, const char *event, void *data);

/*
** Initialize L1 connection.
** For mock: pass NULL or "mock" as endpoint
** For real: pass RPC endpoint like "https://mainnet.infura.io/..."
*/
HppliteL1 *hpplite_l1_connect(const char *endpoint, const char *contractAddress);

/*
** Initialize L1 connection from network config.
** Preferred method - uses structured config.
*/
HppliteL1 *hpplite_l1_connect_config(const HppliteNetworkConfig *config);

/*
** Get system configuration from L1 contract.
** Caller must free with hpplite_l1_system_config_free().
*/
HppliteSystemConfig *hpplite_l1_get_system_config(HppliteL1 *l1);
/* setCheckpointInterval(uint256): the owner's transaction, its hash (sqlite3_free) or NULL (#44) */
char *hpplite_l1_set_checkpoint_interval(HppliteL1 *l1, uint64_t interval);
/* setSequencer(address) (#17): the owner names the wallet that may hold the lease; the running lease expires.
** The transaction's hash (sqlite3_free) or NULL (not the owner, no such call on this backend) */
char *hpplite_l1_set_sequencer(HppliteL1 *l1, const unsigned char wallet[20]);
/* setLeaseDuration(uint256) (#267): the owner sets how long a claim or renewal runs, a minute to 30 days (the
** contract's bounds). The transaction's hash (sqlite3_free) or NULL (not the owner, out of bounds, no such call) */
char *hpplite_l1_set_lease_duration(HppliteL1 *l1, uint64_t seconds);
/* forceExpireLease() (#267): the owner ends the running lease now, whoever holds it - a dead host's, at failover.
** The transaction's hash (sqlite3_free) or NULL (not the owner, no such call on this backend) */
char *hpplite_l1_force_expire_lease(HppliteL1 *l1);

/*
** Free system config
*/
void hpplite_l1_system_config_free(HppliteSystemConfig *config);

/*
** Set private key for signing L1 transactions.
** Must be called before submitting batches or checkpoints.
*/
int hpplite_l1_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]);
/* A signer whose key lives elsewhere (#17): sig r (32) and s (32), recid 0..3; the wallet it signs for */
typedef int (*HppliteSignFn)(void *arg, const unsigned char hash[32], unsigned char sig[64], int *recid);
int hpplite_l1_set_signer(HppliteL1 *l1, HppliteSignFn fn, void *arg, const unsigned char address[20]);

/*
** Disconnect from L1
*/
void hpplite_l1_disconnect(HppliteL1 *l1);

/*
** Get current L1 state
** Caller must free with hpplite_l1_state_free()
*/
HppliteL1State *hpplite_l1_get_state(HppliteL1 *l1);

/*
** Free L1 state
*/
void hpplite_l1_state_free(HppliteL1State *state);

/* === Lease Management (Singleton Sequencer) === */

/*
** Claim sequencer lease.
** instanceId: unique identifier for this process (32 bytes)
** Returns transaction hash on success, NULL on failure.
**
** Every transaction hash returned by this interface is a string allocated
** with sqlite3_mprintf(); release it with sqlite3_free(), never free().
*/
char *hpplite_l1_claim_sequencer(
    HppliteL1 *l1,
    const unsigned char instanceId[32]
);

/*
** Renew sequencer lease.
** Returns transaction hash on success, NULL on failure.
*/
char *hpplite_l1_renew_lease(
    HppliteL1 *l1,
    const unsigned char instanceId[32]
);

/*
** Give up our sequencer lease (graceful shutdown). The rollup owner's wallet
** expires the lease immediately so the next instance can claim it without
** waiting for LEASE_DURATION. Returns transaction hash on success, NULL on failure.
*/
char *hpplite_l1_release_lease(HppliteL1 *l1, const unsigned char instanceId[32]);

/*
** Check if lease is active for the given instance.
** Returns 1 if active, 0 if expired/not held.
*/
int hpplite_l1_is_lease_active(HppliteL1 *l1);

/*
** Chain id used for signing by every connection made after this call
** (HPP Sepolia 181228 by default; anvil is 31337). The node sets it from
** chainid= / HPPLITE_L1_CHAIN_ID before connecting.
*/
void hpplite_l1_set_default_chain_id(uint64_t chainId);
uint64_t hpplite_l1_default_chain_id(void);

/*
** Addresses of the rollup's coordinator and DA contracts as 0x-hex strings
** (43-byte buffers). Returns 0 on success, -1 when the backend has none (mock).
*/
int hpplite_l1_get_addresses(HppliteL1 *l1, char contractHex[43], char daHex[43]);

/* The wallet this connection signs with (needs a private key). 0 on success. */
int hpplite_l1_get_my_address(HppliteL1 *l1, unsigned char out[20]);

/* The chain this connection is on (0 when the backend is on none: mock, directory) */
uint64_t hpplite_l1_chain_id(HppliteL1 *l1);

/* An ERC-20 balance on that chain (#11): balanceOf(owner) of token, 32 big-endian bytes. 0, or -1 when it cannot be read. */
int hpplite_l1_token_balance(HppliteL1 *l1, const unsigned char token[20], const unsigned char owner[20], uint8_t out[32]);
/* The wallet's ETH balance in wei (#173): 0, or -1 when the backend has none or it cannot be read */
int hpplite_l1_eth_balance(HppliteL1 *l1, double *pWei);

/* A relay (#126, operators/relay): the node's writes go through it as their
** EIP-712 signed forms, paid over x402; the plain transaction only when the
** relay fails and the wallet holds ETH. The handle stays the caller's. */
struct HppliteStore;
void hpplite_l1_set_relay(HppliteL1 *l1, struct HppliteStore *relay);
/* "relay" or "direct": how the last write went (NULL: none yet, or a backend that submits nothing) */
const char *hpplite_l1_last_tx_path(HppliteL1 *l1);
/* Why the last relay attempt failed (NULL: no relay, or it succeeded since) */
const char *hpplite_l1_relay_error(HppliteL1 *l1);
/* The wallet's rollup created through the relay (createRollupFor, VERSION 10
** factories): 0; -2 create it from the wallet (no signed form, or the relay
** failed and the wallet holds ETH); -1 the relay failed and the wallet cannot pay */
int hpplite_l1_factory_create_rollup_for(const char *rpc_url, const char *factory_address, const uint8_t privkey[32],
                                         struct HppliteStore *relay, uint64_t checkpointInterval, uint8_t rollup_out[20]);

/*
** Which backend a connection is on: "eth", "dir" or "mock" (#35). Every
** build has all three; the endpoint given to hpplite_l1_connect() and the
** factory calls picks one: "mock" is the mock, "file:///path" the directory
** (l1_dir.c: lease, checkpoints and batches as files, no chain, no wallet),
** anything else the Ethereum client.
*/
const char *hpplite_l1_backend_name(HppliteL1 *l1);


/*
** Get remaining lease time in seconds.
** Returns 0 if no lease or expired.
*/
uint64_t hpplite_l1_get_lease_time_remaining(HppliteL1 *l1);

/* === Checkpoint Operations === */

/*
** Submit a checkpoint to L1 (sequencer only).
** instanceId: must match active lease
** Returns transaction hash on success, NULL on failure.
*/
char *hpplite_l1_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const unsigned char instanceId[32]
);

/*
** Set callback for L1 events
*/
void hpplite_l1_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg);

/*
** Poll for L1 updates (call periodically)
** Returns number of new events, or -1 on error
*/
int hpplite_l1_poll(HppliteL1 *l1);

/*
** Wait for transaction to be mined
** tx_hash is hex string (0x prefixed)
** Returns 1 if success, 0 if reverted, -1 if pending/error
*/
int hpplite_l1_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs);

/* === Factory Functions (for auto-deploy) === */

/*
** Get rollup address for an owner from factory.
** Returns: 1 if rollup exists (address in rollup_address_out)
**          0 if no rollup for this owner
**         -1 on error
*/
int hpplite_l1_factory_get_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t rollup_address_out[20]
);

/*
** Check if owner has a rollup.
** Returns: 1 if rollup exists, 0 if no rollup, -1 on error
*/
int hpplite_l1_factory_has_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20]
);

/*
** Get or create rollup for the wallet associated with privkey.
** Returns transaction hash on success, NULL on failure.
*/
char *hpplite_l1_factory_get_or_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
);

/*
** Several rollups per wallet (#61: one per named database). List the
** owner's rollups, oldest first: returns how many were written (at most
** max), -1 on error. Create one more: waits for it, 1 with its address in
** rollup_out, 0 on failure.
*/
int hpplite_l1_factory_list_rollups(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t (*out)[20],
    int max
);
int hpplite_l1_factory_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint64_t checkpointInterval,
    uint8_t rollup_out[20]
);
int hpplite_l1_factory_list_my_rollups(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t (*out)[20],
    int max
);

/*
** Get rollup address for the wallet associated with privkey.
** Returns: 1 if rollup exists, 0 if no rollup, -1 on error
*/
int hpplite_l1_factory_get_my_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
);

/*
** Connect to L1 using factory - auto-discovers rollup for this wallet.
** Returns NULL if no rollup exists for this wallet.
*/
HppliteL1 *hpplite_l1_connect_factory(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32]
);

/* === Batch DA Functions (on-chain data availability) === */

/*
** Submit batch data to L1 for data availability.
** instanceId: must match active lease
** Returns transaction hash on success, NULL on failure.
*/
char *hpplite_l1_submit_batch(
    HppliteL1 *l1,
    const uint8_t instanceId[32],
    uint64_t height,
    const uint8_t *data,
    size_t data_len
);

/*
** Get batch data from L1.
** Returns batch data (caller must free), NULL if not found.
*/
uint8_t *hpplite_l1_get_batch(
    HppliteL1 *l1,
    uint64_t height,
    size_t *data_len_out
);

/*
** Get batch hash from L1.
** Returns: 1 if batch exists, 0 if no batch, -1 on error
*/
int hpplite_l1_get_batch_hash(
    HppliteL1 *l1,
    uint64_t height,
    uint8_t hash_out[32]
);

/*
** Get DA state from L1 contract.
** Returns 0 on success, -1 on error.
*/
int hpplite_l1_get_da_state(
    HppliteL1 *l1,
    uint64_t *last_batch_height,
    uint64_t *total_batches,
    uint8_t latest_batch_hash[32]
);

/*
** The store's rolling hash over every batch (#33): DA v2 keeps
** keccak256(abi.encode(prev, height, keccak256(bytes))) from bytes32(0),
** and a reader recomputes it (hpplite_chain_hash_next) to compare at the
** tip. 0 with the hash and the height it covers; -1 when the backend keeps
** none (the mock, a directory, DA v1).
*/
int hpplite_l1_get_chain_hash(HppliteL1 *l1, uint64_t *pHeight, uint8_t out[32]);

/*
** A batch whose bytes live in a store (#11): the chain records its hash, a
** reference into the store and the provider's receipt (DA v2 commitBatch).
** commit_batch returns the transaction hash as submit_batch does, NULL when
** refused or unsupported. get_commitment reads a height's commitment back:
** 0 with the hash, the reference (sqlite3_malloc'd) and the receipt bytes
** (sqlite3_malloc'd, may be NULL with length 0), -1 when the height holds
** none or the backend keeps none (the mock, a directory, DA v1). The
** receipt's form is the store's (store.h: a pin service's compact receipt).
*/
char *hpplite_l1_commit_batch(HppliteL1 *l1, const uint8_t instanceId[32], uint64_t height, const uint8_t hash[32],
                              const char *zRef, const uint8_t *receipt, size_t nReceipt);
int hpplite_l1_get_commitment(HppliteL1 *l1, uint64_t height, uint8_t hash[32], char **pzRef, uint8_t **pReceipt, size_t *pnReceipt);

/*
** A range commitment (#164, DA v3): one transaction anchoring every batch a
** store holds since the last commitment. The chain hash after the range is
** the node's fold over its batches (hpplite_chain_hash_next), the state
** root the one after toHeight (the anchor is the checkpoint), the manifest
** digest the sha-256 of the manifest at the store that names every batch in
** the range, the receipt the store's promise to hold the manifest.
** commit_range returns the transaction hash, NULL when refused or when the
** contract has no ranges (DA v2 and before). get_range fills the range a
** height is inside: 0, or -1 when it is inside none (a per-batch
** commitment: get_batch / get_commitment) or the backend keeps none.
** hpplite_l1_range_clear frees what get_range allocated (the receipt).
*/
typedef struct HppliteRange {
    uint64_t fromHeight, toHeight;
    uint8_t chainHashAfter[32];
    uint8_t stateRoot[32];
    uint8_t manifestDigest[32];
    uint8_t *receipt;        /* sqlite3_malloc'd, NULL when none */
    size_t nReceipt;
} HppliteRange;
char *hpplite_l1_commit_range(HppliteL1 *l1, const uint8_t instanceId[32], const HppliteRange *r);
int hpplite_l1_get_range(HppliteL1 *l1, uint64_t height, HppliteRange *out);
void hpplite_l1_range_clear(HppliteRange *r);
int hpplite_l1_has_ranges(HppliteL1 *l1);   /* 1 when the chain takes range commitments */

/* === Mock-specific functions for testing === */

/*
** Create a mock L1 with custom initial state
*/
HppliteL1 *hpplite_l1_create_mock(void);

/*
** Mock L1 whose state lives in a directory (batches, lease, checkpoints,
** clock), reloaded on create: survives the process, for crash tests and
** chain-free use. The directory must exist.
*/
HppliteL1 *hpplite_l1_create_mock_dir(const char *dir);

/*
** Set mock sequencer (for testing)
*/
void hpplite_l1_mock_set_sequencer(HppliteL1 *l1, const unsigned char *pubkey);

/*
** Advance mock block number (for testing timeouts)
*/
void hpplite_l1_mock_advance_blocks(HppliteL1 *l1, uint64_t blocks);

/*
** Set mock configuration
*/
void hpplite_l1_mock_set_config(
    HppliteL1 *l1,
    uint64_t checkpointInterval,
    uint64_t leaseDuration
);

/*
** Advance mock time directly (in seconds)
*/
void hpplite_l1_mock_advance_time(HppliteL1 *l1, uint64_t seconds);

/*
** Get mock current timestamp
*/
uint64_t hpplite_l1_mock_get_timestamp(HppliteL1 *l1);

/*
** Get mock current block
*/
uint64_t hpplite_l1_mock_get_block(HppliteL1 *l1);

/*
** Make hpplite_l1_submit_batch() fail (for testing retry behaviour)
*/
void hpplite_l1_mock_set_fail_submit(HppliteL1 *l1, int fail);
/* The RPC's transaction size limit, below the contract's batch limit: larger
** submissions fail as the real RPC's "oversized data" does */
void hpplite_l1_mock_set_max_tx_bytes(HppliteL1 *l1, size_t n);
/* Did the last hpplite_l1_submit_batch() fail because the transaction was too
** large for the chain - its RPC's size limit, or more gas than a transaction
** may use (never sent; posting smaller batches will work)? */
int hpplite_l1_submit_was_oversized(HppliteL1 *l1);
/* The chain's gas cap per transaction (0 = unknown): a batch whose estimate
** is above it is not sent, a sent one never asks for more */
void hpplite_l1_set_max_tx_gas(HppliteL1 *l1, uint64_t gas);

/* A chain reorganisation: batches above height `keep` are gone */
void hpplite_l1_mock_truncate(HppliteL1 *l1, uint64_t keep);

/* Accept n more submissions, then fail them all (a crash in the middle of a series) */
void hpplite_l1_mock_set_fail_submit_after(HppliteL1 *l1, int n);

/* A lease held by someone else: wallet (NULL = a stranger) with instanceId, for `seconds` */
void hpplite_l1_mock_claim_foreign(HppliteL1 *l1, const unsigned char *wallet,
                                   const unsigned char instanceId[32], uint64_t seconds);
/* Append raw bytes as the next batch, bypassing the lease (a foreign poster) */
void hpplite_l1_mock_inject_batch(HppliteL1 *l1, const uint8_t *data, size_t len);

/* A claim that is mined and reverts (another instance got there first): the
** caller gets a transaction hash, hpplite_l1_wait_for_tx() says 0, the lease
** is not taken */
void hpplite_l1_mock_set_claim_reverts(HppliteL1 *l1, int revert);
/* The endpoint is unreachable: every call fails until it is set back */
void hpplite_l1_mock_set_rpc_down(HppliteL1 *l1, int down);
/* Make hpplite_l1_wait_for_tx() take this long (a flush that is in flight) */
void hpplite_l1_mock_set_submit_delay_ms(HppliteL1 *l1, int ms);

/*
** Store submitted batches but report failure, as if the RPC reply was lost
** (for testing the adopt path)
*/
void hpplite_l1_mock_set_lose_confirmation(HppliteL1 *l1, int lose);

/*
** Make hpplite_l1_wait_for_tx() report the transaction as still pending
** (-1), as on a slow or stalled chain
*/
void hpplite_l1_mock_set_tx_pending(HppliteL1 *l1, int pending);

/*
** Batch size limit enforced by the mock DA (default 128 KB)
*/
void hpplite_l1_mock_set_batch_size_limit(HppliteL1 *l1, size_t limit);

/*
** Get last checkpoint (for testing)
*/
const HppliteCheckpoint *hpplite_l1_mock_get_last_checkpoint(HppliteL1 *l1);

/*
** Get checkpoint count (for testing)
*/
int hpplite_l1_mock_get_checkpoint_count(HppliteL1 *l1);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_L1_INTERFACE_H */
