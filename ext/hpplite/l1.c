/*
** HPPLite L1 dispatch (#35)
**
** The public L1 functions forward to the backend behind a connection
** (l1->ops), or, for the calls made before there is a connection, to the
** backend the endpoint names: "mock" is the in-memory mock, "file:///path"
** the directory backend, anything else the Ethereum client. One build
** carries every backend.
*/

#include "l1_backend.h"
#include "hpplite_sqlite.h"
#include <string.h>

static const HppliteL1Ops *backendFor(const char *endpoint) {
    if (endpoint && strcmp(endpoint, "mock") == 0) return &hppliteL1MockOps;
    if (endpoint && strncmp(endpoint, "file://", 7) == 0) return &hppliteL1DirOps;
    return &hppliteL1EthOps;
}

const char *hpplite_l1_backend_name(HppliteL1 *l1) {
    return l1 ? l1->ops->name : NULL;
}

/* Chain id for signing, shared by every backend (anvil is 31337) */
static uint64_t g_defaultChainId = 181228;
void hpplite_l1_set_default_chain_id(uint64_t chainId) { if (chainId) g_defaultChainId = chainId; }
uint64_t hpplite_l1_default_chain_id(void) { return g_defaultChainId; }

void hpplite_l1_system_config_free(HppliteSystemConfig *config) {
    if (!config) return;
    sqlite3_free(config->daScheme);
    sqlite3_free(config);
}

void hpplite_l1_state_free(HppliteL1State *state) {
    sqlite3_free(state);
}

/* === Connections === */

HppliteL1 *hpplite_l1_connect(const char *endpoint, const char *contractAddress) {
    return backendFor(endpoint)->connect(endpoint, contractAddress);
}

HppliteL1 *hpplite_l1_connect_config(const HppliteNetworkConfig *config) {
    return backendFor(config ? config->rpcUrl : NULL)->connect_config(config);
}

HppliteL1 *hpplite_l1_connect_factory(const char *rpc_url, const char *factory_address, const uint8_t privkey[32]) {
    return backendFor(rpc_url)->connect_factory(rpc_url, factory_address, privkey);
}

void hpplite_l1_disconnect(HppliteL1 *l1) {
    if (l1) l1->ops->disconnect(l1);
}

/* === Coordinator === */

HppliteSystemConfig *hpplite_l1_get_system_config(HppliteL1 *l1) {
    return l1 ? l1->ops->get_system_config(l1) : NULL;
}

char *hpplite_l1_set_checkpoint_interval(HppliteL1 *l1, uint64_t interval) {
    return l1 ? l1->ops->set_checkpoint_interval(l1, interval) : NULL;
}

char *hpplite_l1_set_sequencer(HppliteL1 *l1, const unsigned char wallet[20]) {
    return (l1 && l1->ops->set_sequencer) ? l1->ops->set_sequencer(l1, wallet) : NULL;
}

int hpplite_l1_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]) {
    return l1 ? l1->ops->set_privkey(l1, privkey) : -1;
}

int hpplite_l1_set_signer(HppliteL1 *l1, HppliteSignFn fn, void *arg, const unsigned char address[20]) {
    return (l1 && l1->ops->set_signer) ? l1->ops->set_signer(l1, fn, arg, address) : -1;
}

HppliteL1State *hpplite_l1_get_state(HppliteL1 *l1) {
    return l1 ? l1->ops->get_state(l1) : NULL;
}

char *hpplite_l1_claim_sequencer(HppliteL1 *l1, const unsigned char instanceId[32]) {
    return l1 ? l1->ops->claim_sequencer(l1, instanceId) : NULL;
}

char *hpplite_l1_renew_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    return l1 ? l1->ops->renew_lease(l1, instanceId) : NULL;
}

char *hpplite_l1_release_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    return l1 ? l1->ops->release_lease(l1, instanceId) : NULL;
}

int hpplite_l1_is_lease_active(HppliteL1 *l1) {
    return l1 ? l1->ops->is_lease_active(l1) : 0;
}

int hpplite_l1_get_addresses(HppliteL1 *l1, char contractHex[43], char daHex[43]) {
    return l1 ? l1->ops->get_addresses(l1, contractHex, daHex) : -1;
}

int hpplite_l1_get_my_address(HppliteL1 *l1, unsigned char out[20]) {
    return l1 ? l1->ops->get_my_address(l1, out) : -1;
}

uint64_t hpplite_l1_chain_id(HppliteL1 *l1) {
    return (l1 && l1->ops->chain_id) ? l1->ops->chain_id(l1) : 0;
}

void hpplite_l1_set_relay(HppliteL1 *l1, struct HppliteStore *relay) {
    if (l1 && l1->ops->set_relay) l1->ops->set_relay(l1, relay);
}

const char *hpplite_l1_last_tx_path(HppliteL1 *l1) {
    return (l1 && l1->ops->last_tx_path) ? l1->ops->last_tx_path(l1) : NULL;
}

const char *hpplite_l1_relay_error(HppliteL1 *l1) {
    return (l1 && l1->ops->relay_error) ? l1->ops->relay_error(l1) : NULL;
}

int hpplite_l1_factory_create_rollup_for(const char *rpc_url, const char *factory_address, const uint8_t privkey[32],
                                         struct HppliteStore *relay, uint64_t checkpointInterval, uint8_t rollup_out[20]) {
    const HppliteL1Ops *ops = backendFor(rpc_url);
    return ops->factory_create_rollup_for ? ops->factory_create_rollup_for(rpc_url, factory_address, privkey, relay, checkpointInterval, rollup_out) : -2;
}

int hpplite_l1_token_balance(HppliteL1 *l1, const unsigned char token[20], const unsigned char owner[20], uint8_t out[32]) {
    return (l1 && l1->ops->token_balance) ? l1->ops->token_balance(l1, token, owner, out) : -1;
}

int hpplite_l1_eth_balance(HppliteL1 *l1, double *pWei) {
    return (l1 && pWei && l1->ops->eth_balance) ? l1->ops->eth_balance(l1, pWei) : -1;
}

uint64_t hpplite_l1_get_lease_time_remaining(HppliteL1 *l1) {
    return l1 ? l1->ops->get_lease_time_remaining(l1) : 0;
}

char *hpplite_l1_submit_checkpoint(HppliteL1 *l1, const HppliteCheckpoint *checkpoint, const unsigned char instanceId[32]) {
    return l1 ? l1->ops->submit_checkpoint(l1, checkpoint, instanceId) : NULL;
}

void hpplite_l1_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg) {
    if (l1) l1->ops->set_callback(l1, callback, arg);
}

int hpplite_l1_poll(HppliteL1 *l1) {
    return l1 ? l1->ops->poll(l1) : -1;
}

int hpplite_l1_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs) {
    return l1 ? l1->ops->wait_for_tx(l1, tx_hash, timeout_secs) : -1;
}

/* === Factory === */

int hpplite_l1_factory_get_rollup(const char *rpc_url, const char *factory_address,
                                  const uint8_t owner_address[20], uint8_t rollup_address_out[20]) {
    return backendFor(rpc_url)->factory_get_rollup(rpc_url, factory_address, owner_address, rollup_address_out);
}

int hpplite_l1_factory_has_rollup(const char *rpc_url, const char *factory_address, const uint8_t owner_address[20]) {
    return backendFor(rpc_url)->factory_has_rollup(rpc_url, factory_address, owner_address);
}

char *hpplite_l1_factory_get_or_create_rollup(const char *rpc_url, const char *factory_address,
                                              const uint8_t privkey[32], uint8_t rollup_address_out[20]) {
    return backendFor(rpc_url)->factory_get_or_create_rollup(rpc_url, factory_address, privkey, rollup_address_out);
}

int hpplite_l1_factory_list_rollups(const char *rpc_url, const char *factory_address,
                                    const uint8_t owner_address[20], uint8_t (*out)[20], int max) {
    return backendFor(rpc_url)->factory_list_rollups(rpc_url, factory_address, owner_address, out, max);
}

int hpplite_l1_factory_create_rollup(const char *rpc_url, const char *factory_address, const uint8_t privkey[32],
                                     uint64_t checkpointInterval, uint8_t rollup_out[20]) {
    return backendFor(rpc_url)->factory_create_rollup(rpc_url, factory_address, privkey, checkpointInterval, rollup_out);
}

int hpplite_l1_factory_list_my_rollups(const char *rpc_url, const char *factory_address,
                                       const uint8_t privkey[32], uint8_t (*out)[20], int max) {
    return backendFor(rpc_url)->factory_list_my_rollups(rpc_url, factory_address, privkey, out, max);
}

int hpplite_l1_factory_get_my_rollup(const char *rpc_url, const char *factory_address,
                                     const uint8_t privkey[32], uint8_t rollup_address_out[20]) {
    return backendFor(rpc_url)->factory_get_my_rollup(rpc_url, factory_address, privkey, rollup_address_out);
}

/* === Store === */

char *hpplite_l1_submit_batch(HppliteL1 *l1, const uint8_t instanceId[32], uint64_t height,
                              const uint8_t *data, size_t data_len) {
    return l1 ? l1->ops->submit_batch(l1, instanceId, height, data, data_len) : NULL;
}

uint8_t *hpplite_l1_get_batch(HppliteL1 *l1, uint64_t height, size_t *data_len_out) {
    return l1 ? l1->ops->get_batch(l1, height, data_len_out) : NULL;
}

int hpplite_l1_get_batch_hash(HppliteL1 *l1, uint64_t height, uint8_t hash_out[32]) {
    return l1 ? l1->ops->get_batch_hash(l1, height, hash_out) : -1;
}

int hpplite_l1_get_da_state(HppliteL1 *l1, uint64_t *last_batch_height, uint64_t *total_batches,
                            uint8_t latest_batch_hash[32]) {
    return l1 ? l1->ops->get_da_state(l1, last_batch_height, total_batches, latest_batch_hash) : -1;
}

int hpplite_l1_submit_was_oversized(HppliteL1 *l1) {
    return l1 ? l1->ops->submit_was_oversized(l1) : 0;
}

void hpplite_l1_set_max_tx_gas(HppliteL1 *l1, uint64_t gas) {
    if (l1) l1->ops->set_max_tx_gas(l1, gas);
}

int hpplite_l1_get_chain_hash(HppliteL1 *l1, uint64_t *pHeight, uint8_t out[32]) {
    return l1 ? l1->ops->get_chain_hash(l1, pHeight, out) : -1;
}

char *hpplite_l1_commit_batch(HppliteL1 *l1, const uint8_t instanceId[32], uint64_t height, const uint8_t hash[32],
                              const char *zRef, const uint8_t *receipt, size_t nReceipt) {
    return (l1 && l1->ops->commit_batch) ? l1->ops->commit_batch(l1, instanceId, height, hash, zRef, receipt, nReceipt) : NULL;
}

int hpplite_l1_get_commitment(HppliteL1 *l1, uint64_t height, uint8_t hash[32], char **pzRef, uint8_t **pReceipt, size_t *pnReceipt) {
    if (pzRef) *pzRef = NULL;
    if (pReceipt) *pReceipt = NULL;
    if (pnReceipt) *pnReceipt = 0;
    return (l1 && l1->ops->get_commitment) ? l1->ops->get_commitment(l1, height, hash, pzRef, pReceipt, pnReceipt) : -1;
}

char *hpplite_l1_commit_range(HppliteL1 *l1, const uint8_t instanceId[32], const HppliteRange *r) {
    return (l1 && r && l1->ops->commit_range) ? l1->ops->commit_range(l1, instanceId, r) : NULL;
}

int hpplite_l1_get_range(HppliteL1 *l1, uint64_t height, HppliteRange *out) {
    if (out) memset(out, 0, sizeof *out);
    return (l1 && out && l1->ops->get_range) ? l1->ops->get_range(l1, height, out) : -1;
}

int hpplite_l1_has_ranges(HppliteL1 *l1) {
    return (l1 && l1->ops->has_ranges) ? l1->ops->has_ranges(l1) : 0;
}

void hpplite_l1_range_clear(HppliteRange *r) {
    if (!r) return;
    sqlite3_free(r->receipt);
    r->receipt = NULL;
    r->nReceipt = 0;
}
