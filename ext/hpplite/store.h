/*
** HPPLite stores (#11): where a batch's bytes live when they are not on the
** chain. The chain keeps the commitment (hash, reference, receipt; #33),
** the store keeps the bytes: put bytes, get by reference. The reference is
** opaque to the node (a CID at a pin service).
**
** store=http://host:port is the pin service of examples/pin_service (or any
** provider speaking its four calls): POST /put, GET /get/<ref>. A 402 is
** paid in x402's exact scheme with the node's key, up to storecap= per call.
*/
#ifndef HPPLITE_STORE_H
#define HPPLITE_STORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct HppliteStore HppliteStore;

/* Open a store by URL; NULL when the scheme is unknown. The key (may be
** NULL) pays for calls the provider charges for; cap is the most to pay per
** call in the asset's atomic units (0: pay nothing, refuse every 402). */
HppliteStore *hpplite_store_open(const char *url, const uint8_t *privkey, uint64_t cap);
void hpplite_store_close(HppliteStore *store);

/* Put bytes: the reference (sqlite3_mprintf, caller frees) and, when the
** provider signed one, its receipt as compact JSON (sqlite3_mprintf, or
** NULL). NULL on failure; hpplite_store_error() says why. */
char *hpplite_store_put(HppliteStore *store, const uint8_t *data, size_t len, char **pzReceipt);

/* Get the bytes a reference names: malloc'd, terminated, *pLen set; NULL when not there */
uint8_t *hpplite_store_get(HppliteStore *store, const char *zRef, size_t *pLen);

const char *hpplite_store_error(HppliteStore *store);
const char *hpplite_store_url(HppliteStore *store);

/* How much the store paid so far (atomic units) and for how many paid calls */
uint64_t hpplite_store_spent(HppliteStore *store, int *pCalls);
/* Puts and gets that succeeded, paid or not; the last put's size */
int hpplite_store_calls(HppliteStore *store);
size_t hpplite_store_last_bytes(HppliteStore *store);
/* What the provider charges, from its /status: the asset, its chain, the price per byte and per
** pin (a put costs perPin + perByte * bytes). -1 when it charges nothing or said nothing. */
int hpplite_store_payment(HppliteStore *store, unsigned char asset[20], uint64_t *pChainId, uint64_t *pPerByte, uint64_t *pPerPin);

/* Seconds a payment authorization stays valid (default 120; a test chain whose clock ran ahead needs more) */
void hpplite_store_set_valid_for(HppliteStore *store, uint64_t seconds);

/*
** A pin service's receipt, as the chain carries it (#33): 94 bytes,
** version 1 | signer (20) | expires (8, big-endian seconds) | signature (65),
** the provider's EIP-712 signature over Receipt(bytes32 hash, string cid,
** uint256 expires) in its domain. Compact from the JSON the service
** answers (94, or -1 when it is not a receipt); decode back for display
** and verification.
*/
#define HPPLITE_RECEIPT_SIZE 114
/* version 1 | signer (20) | payTo (20, the receipt domain's verifyingContract) | expires (8) | signature (65) */
int hpplite_store_receipt_compact(const char *zReceiptJson, const unsigned char payTo[20], uint8_t out[HPPLITE_RECEIPT_SIZE]);
int hpplite_store_receipt_decode(const uint8_t *receipt, size_t n, unsigned char signer[20], unsigned char payTo[20],
                                 uint64_t *pExpires, unsigned char sig[65]);
/* The receipt's signature recovers to its signer over (hash, cid, expires) in the domain
** ("HPPLitePinService", "1", chainId, payTo); and, when expected is given, that signer is
** the expected one. 0 verified, -1 not. */
int hpplite_store_receipt_verify(const uint8_t *receipt, size_t n, const unsigned char hash[32], const char *zCid,
                                 uint64_t chainId, const unsigned char *expectedSigner);

/* The provider's published signer and receipt domain (from its /status at open); 0 with them, -1 when the provider named none */
int hpplite_store_signer(HppliteStore *store, unsigned char out[20]);
int hpplite_store_domain(HppliteStore *store, unsigned char payTo[20], uint64_t *pChainId);

/* Why the last put or get failed, for the node's choice between waiting and posting on chain */
#define HPPLITE_STORE_FAIL_NONE        0
#define HPPLITE_STORE_FAIL_UNREACHABLE 1   /* no answer: the chain can carry the bytes meanwhile */
#define HPPLITE_STORE_FAIL_CAP         2   /* the price is over storecap=: nothing signed, the batch waits */
#define HPPLITE_STORE_FAIL_RECEIPT     3   /* the receipt does not verify: the provider is not to be trusted with this batch */
#define HPPLITE_STORE_FAIL_OTHER       4
int hpplite_store_last_failure(HppliteStore *store);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_STORE_H */
