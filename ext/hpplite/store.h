/*
** HPPLite stores (#11): where a batch's bytes live when they are not on the
** chain. The chain keeps the commitment (hash, reference, receipt; #33),
** the store keeps the bytes: put bytes, get by reference. The reference is
** opaque to the node (a CID at a pin service).
**
** store=http://host:port is the pin service of examples/pin_service (or any
** provider speaking its four calls): POST /put, GET /get/<ref>. A 402 is
** paid in x402's exact scheme with the node's key, up to storecap= per call.
** store=kubo://host:port is a Kubo node's own RPC (add, cat): the node pins
** what it is given and signs nothing, so the chain commits (hash, CID) with
** no receipt. store=s3://bucket[/prefix] is a bucket over Signature Version
** 4 (AWS_ACCESS_KEY_ID, AWS_SECRET_ACCESS_KEY, AWS_REGION; HPPLITE_S3_ENDPOINT
** for a compatible service), keyed by the bytes' CID under the prefix; the
** same (hash, CID) commitment, no receipt.
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

/* The key given after open (#17): pays from here on. 0, or -1 when it is not a key */
int hpplite_store_set_key(HppliteStore *store, const uint8_t privkey[32]);
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
/* The name a store gives bytes whose sha-256 is `digest`: CIDv1, raw, base32 (what put returns for them);
** a reader that holds a digest (#164: the manifest's, from the chain) asks the store for this */
void hpplite_store_cid_from_sha256(const uint8_t digest[32], char out[64]);
void hpplite_store_sha256(const uint8_t *data, size_t len, uint8_t out[32]);

/* Named entries (#165): a batch under its rollup's name "<da hex, no 0x>-<height>" with the
** sequencer instance (32 bytes) and its EIP-712 Batch signature (65 bytes, what
** HPPLiteDA.batchDigest signs: instanceId, height, keccak256 of the bytes, in the DA contract's
** domain) appended, so a reader finds batches by height and recovers who claims them (#167).
** sign fills sig from the store's key (-1 without one); put_named stores bytes||instance||sig
** under the name (0, or -1 with the reason in error); get_named reads them back, the instance
** and the signature apart (malloc'd bytes, or NULL). A pin service and an S3 bucket take
** names; a Kubo node does not (-1 / NULL). */
int hpplite_store_sign_batch(HppliteStore *store, const unsigned char da[20], uint64_t chainId, const unsigned char instance[32],
                             uint64_t height, const uint8_t *data, size_t len, uint8_t sig[65]);
int hpplite_store_put_named(HppliteStore *store, const char *zName, const unsigned char instance[32], const uint8_t *data, size_t len, const uint8_t sig[65]);
uint8_t *hpplite_store_get_named(HppliteStore *store, const char *zName, size_t *pLen, uint8_t sig[65], unsigned char instance[32]);
/* The wallet a named entry's signature recovers to (0), or -1 when it does not recover */
int hpplite_store_batch_signer(const unsigned char da[20], uint64_t chainId, const unsigned char instance[32], uint64_t height,
                               const uint8_t *data, size_t len, const uint8_t sig[65], unsigned char out[20]);
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
