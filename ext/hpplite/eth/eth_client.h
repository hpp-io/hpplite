/*
** Minimal Ethereum JSON-RPC client for HPPLite
** Uses libcurl for HTTP, cJSON for JSON parsing
*/

#ifndef ETH_CLIENT_H
#define ETH_CLIENT_H

#include <stdint.h>
#include <stddef.h>

/* Client handle */
typedef struct EthClient EthClient;

/* Create client connected to RPC endpoint */
/* Process-wide curl/secp256k1 setup; called by eth_client_create(), safe to call early from any thread */
void eth_client_global_init(void);
EthClient *eth_client_create(const char *rpc_url, uint64_t chain_id);

/* Destroy client */
void eth_client_destroy(EthClient *client);

/* Set private key for signing transactions */
int eth_client_set_privkey(EthClient *client, const uint8_t privkey[32]);
/* A signer that holds the key elsewhere (a KMS, an HSM, #17): sig is r (32) and s (32), recid 0..3; 0 signed */
typedef int (*EthSignFn)(void *arg, const uint8_t hash[32], uint8_t sig[64], int *recid);
int eth_client_set_signer(EthClient *client, EthSignFn fn, void *arg, const uint8_t address[20]);

/* Get chain ID */
uint64_t eth_client_chain_id(EthClient *client);

/* === Read operations (eth_call) === */

/* Call contract function, returns allocated result (caller frees) */
uint8_t *eth_client_call(EthClient *client,
                         const uint8_t to[20],
                         const uint8_t *data, size_t data_len,
                         size_t *result_len);

/* === Write operations === */

/* Get nonce for address */
uint64_t eth_client_get_nonce(EthClient *client, const uint8_t address[20]);

/* eth_estimateGas from our address; (uint64_t)-1 on failure */
uint64_t eth_client_estimate_gas(EthClient *client, const uint8_t to[20], const uint8_t *data, size_t data_len);

/* Sign a legacy (EIP-155) transaction into out; returns its length, 0 on error. For tests. */
size_t eth_client_sign_tx(EthClient *client, uint64_t nonce, uint64_t gas_price, uint64_t gas_limit,
                          const uint8_t to[20], uint64_t value, const uint8_t *data, size_t data_len,
                          uint8_t *out, size_t out_size);

/* Latest block number and timestamp (the chain's clock). 0 on success. */
int eth_client_latest_block(EthClient *client, uint64_t *pNumber, uint64_t *pTimestamp);

/* Get current gas price */
uint64_t eth_client_gas_price(EthClient *client);

/* Send raw transaction, returns tx hash (32 bytes) or NULL on error */
uint8_t *eth_client_send_raw_tx(EthClient *client,
                                const uint8_t *raw_tx, size_t tx_len);

/* Send transaction (builds, signs, sends)
   Returns tx hash (32 bytes, caller frees) or NULL on error */
uint8_t *eth_client_send_tx(EthClient *client,
                            const uint8_t to[20],
                            const uint8_t *data, size_t data_len,
                            uint64_t gas_limit,
                            uint64_t value);

/* Get transaction receipt, returns 1 if mined, 0 if pending, -1 on error
   If mined, fills in status (1 = success, 0 = reverted) */
/* Mined: the given topic of the first log whose topic 0 is topic0 -> out;
** 1 found, 0 not mined yet, -1 mined without such a log (reverted), -2 the
** RPC call failed (the transaction may still land: ask again) */
int eth_client_receipt_topic(EthClient *client, const uint8_t tx_hash[32],
                             const uint8_t topic0[32], int index, uint8_t out[32]);

int eth_client_get_receipt(EthClient *client,
                           const uint8_t tx_hash[32],
                           int *status);

/* eth_getLogs for one block: the first log of `address` in `block` whose
** topic 0 is topic0 and topic 1 is topic1 (#33: a batch's bytes are in its
** log). On success its ABI-encoded data is returned (malloc, *data_len set)
** and, when topic2 is given, its third topic. NULL when there is no such
** log or the call failed (eth_client_error() says which). */
uint8_t *eth_client_get_log(EthClient *client, const uint8_t address[20], uint64_t block,
                            const uint8_t topic0[32], const uint8_t topic1[32],
                            size_t *data_len, uint8_t topic2[32]);

/* === Utility === */

/* Get last error message */
const char *eth_client_error(EthClient *client);

/* Hex encode/decode utilities */
char *eth_hex_encode(const uint8_t *data, size_t len);
int eth_hex_decode(const char *hex, uint8_t *out, size_t out_len);
/* Hex of any even length (0x or not) to bytes: the count, or -1 on an odd length, a bad digit or more than max bytes */
int eth_hex_decode_any(const char *hex, uint8_t *out, size_t max);
/* Lowercase hex without 0x into a caller's buffer of 2 * len + 1 */
void eth_hex_encode_to(const uint8_t *data, size_t len, char *out);

/* Derive Ethereum address from compressed (33-byte) public key */
int eth_address_from_compressed_pubkey(const uint8_t pubkey[33], uint8_t address[20]);

/* Derive Ethereum address from private key */
int eth_address_from_privkey(const uint8_t privkey[32], uint8_t address[20]);

#endif /* ETH_CLIENT_H */
