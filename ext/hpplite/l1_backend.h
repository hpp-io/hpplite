/*
** HPPLite L1 backends (#35)
**
** The public functions in l1_interface.h are implemented once, in l1.c, by
** forwarding to a backend's function table. Every build links every backend;
** the endpoint picks one at run time: "mock" is the in-memory mock (and its
** directory-backed variant), anything else is the Ethereum client. A backend's
** connection struct begins with HppliteL1, whose only member is the table.
**
** Two halves, which the directory and off-chain backends (#35, #11) separate:
** the coordinator (lease, checkpoints, system config, the rollup for a key)
** and the store (submit_batch, get_batch, get_batch_hash, get_da_state).
*/

#ifndef HPPLITE_L1_BACKEND_H
#define HPPLITE_L1_BACKEND_H

#include "l1_interface.h"

typedef struct HppliteL1Ops HppliteL1Ops;

struct HppliteL1 {
    const HppliteL1Ops *ops;
};

struct HppliteL1Ops {
    const char *name;                 /* "eth", "mock" */

    /* Connections. connect_config and the factory functions have no
    ** connection yet; the endpoint they are given chose this backend. */
    HppliteL1 *(*connect)(const char *endpoint, const char *contractAddress);
    HppliteL1 *(*connect_config)(const HppliteNetworkConfig *config);
    HppliteL1 *(*connect_factory)(const char *rpc_url, const char *factory_address, const uint8_t privkey[32]);
    void (*disconnect)(HppliteL1 *l1);

    /* Coordinator */
    HppliteSystemConfig *(*get_system_config)(HppliteL1 *l1);
    char *(*set_checkpoint_interval)(HppliteL1 *l1, uint64_t interval);
    int (*set_privkey)(HppliteL1 *l1, const uint8_t privkey[32]);
    HppliteL1State *(*get_state)(HppliteL1 *l1);
    char *(*claim_sequencer)(HppliteL1 *l1, const unsigned char instanceId[32]);
    char *(*renew_lease)(HppliteL1 *l1, const unsigned char instanceId[32]);
    char *(*release_lease)(HppliteL1 *l1, const unsigned char instanceId[32]);
    int (*is_lease_active)(HppliteL1 *l1);
    int (*get_addresses)(HppliteL1 *l1, char contractHex[43], char daHex[43]);
    int (*get_my_address)(HppliteL1 *l1, unsigned char out[20]);
    uint64_t (*get_lease_time_remaining)(HppliteL1 *l1);
    char *(*submit_checkpoint)(HppliteL1 *l1, const HppliteCheckpoint *checkpoint, const unsigned char instanceId[32]);
    void (*set_callback)(HppliteL1 *l1, HppliteL1Callback callback, void *arg);
    int (*poll)(HppliteL1 *l1);
    int (*wait_for_tx)(HppliteL1 *l1, const char *tx_hash, int timeout_secs);

    /* Factory (the rollup for a key), without a connection */
    int (*factory_get_rollup)(const char *rpc_url, const char *factory_address, const uint8_t owner_address[20], uint8_t rollup_address_out[20]);
    int (*factory_has_rollup)(const char *rpc_url, const char *factory_address, const uint8_t owner_address[20]);
    char *(*factory_get_or_create_rollup)(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint8_t rollup_address_out[20]);
    int (*factory_list_rollups)(const char *rpc_url, const char *factory_address, const uint8_t owner_address[20], uint8_t (*out)[20], int max);
    int (*factory_create_rollup)(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint64_t checkpointInterval, uint8_t rollup_out[20]);
    int (*factory_list_my_rollups)(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint8_t (*out)[20], int max);
    int (*factory_get_my_rollup)(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint8_t rollup_address_out[20]);

    /* Store */
    char *(*submit_batch)(HppliteL1 *l1, const uint8_t instanceId[32], uint64_t height, const uint8_t *data, size_t data_len);
    uint8_t *(*get_batch)(HppliteL1 *l1, uint64_t height, size_t *data_len_out);
    int (*get_batch_hash)(HppliteL1 *l1, uint64_t height, uint8_t hash_out[32]);
    int (*get_da_state)(HppliteL1 *l1, uint64_t *last_batch_height, uint64_t *total_batches, uint8_t latest_batch_hash[32]);
    int (*submit_was_oversized)(HppliteL1 *l1);
    void (*set_max_tx_gas)(HppliteL1 *l1, uint64_t gas);
};

extern const HppliteL1Ops hppliteL1EthOps;    /* l1_eth.c */
extern const HppliteL1Ops hppliteL1MockOps;   /* l1_mock.c */

/* HppliteL1State and HppliteSystemConfig (and its daScheme) are allocated
** with sqlite3_malloc()/sqlite3_mprintf() by every backend: l1.c frees them. */

#endif /* HPPLITE_L1_BACKEND_H */
