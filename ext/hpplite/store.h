/*
** HPPLite stores (#11): where a batch's bytes live when they are not on the
** chain. The chain keeps the commitment (hash, reference; #33, and at an anchor the store's bucket statement, #214),
** the store keeps the bytes: put bytes, get by reference. The reference is
** opaque to the node (a CID at a pin service).
**
** store=http(s)://host[:port] is an hpplite-store/2 store (operators/store/hpplite_store.py, or the network's
** canonical one, store=on): the rollup's bucket, paid by its funding wallet. A 402 is paid in x402's exact
** scheme with the node's key, up to HPPLITE_STORE_CAP per call. Nothing else is a store: kubo:// and s3:// were
** removed in v0.8 (#250) - they took no payment, no names (Kubo) and gave no bucket statement.
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

/* POST a body to another endpoint of the same service, paying a 402 as put
** does (the cap, the accounting): the reference relay's /commit (#126).
** The HTTP status (0: unreachable; 402: not paid, the reason in
** hpplite_store_error) and the answer's body (sqlite3_free) */
long hpplite_store_paid_post(HppliteStore *s, const char *path, const uint8_t *body, size_t len, char **pzOut);
/* The handle is a relay's (#126): what it says names the relay and HPPLITE_RELAY_CAP */
void hpplite_store_set_relay(HppliteStore *s);
/* The node's spend ledger (#173, spend.h): checked before signing (HPPLITE_SPEND_CAP), charges recorded. Not owned. */
struct HppliteSpend;
void hpplite_store_set_spend(HppliteStore *s, struct HppliteSpend *sp);

/* Put bytes: the reference (sqlite3_mprintf, caller frees), NULL on failure; hpplite_store_error() says why.
** No per-item receipt (#249: hpplite-store/1's went with /1): the store's word is the bucket statement an anchor carries */
char *hpplite_store_put(HppliteStore *store, const uint8_t *data, size_t len);
/* ...at a batch height: a hpplite-store/2 store takes the bytes into the rollup's bucket, signed for that
** height (what a prune point counts); put is put_at height 0 */
char *hpplite_store_put_at(HppliteStore *store, uint64_t height, const uint8_t *data, size_t len);
/*
** hpplite-store/2 (#214, doc/store-protocol.md): the rollup's bucket at the store, paid from a funding
** wallet whose key is derived from the node's. set_bucket names the rollup (its DA) and what the
** funding wallet's permit lets the store pull (HPPLITE_STORE_PERMIT, atomic units; 0: no permit, the store's
** 402s are paid instead); the bucket is bound and the permit given at the first write. is_bucket: the
** store speaks /2. funder: the funding wallet's address (send USDC.e there). covered_until and
** bucket_state: from the last statement (-1 / NULL when not known). statement: the store's signed
** BucketStatement in the compact form an anchor commits (version 2 | signer | payTo | height |
** pruneHeight | coveredUntil | issuedAt, 8 bytes each | signature), checked against the store's
** signer; its size, or -1.
*/
#define HPPLITE_STATEMENT_SIZE 138
/* coveredUntil when nothing is charged (a free store): 2^53-1, exact in a double, an int64 and any JSON (#216) */
#define HPPLITE_STATEMENT_FOREVER 9007199254740991ULL
void hpplite_store_set_bucket(HppliteStore *store, const unsigned char da[20], uint64_t permitCap);
/* hpplite-store/2: the sequencer's signed Prune - the store lets go of the bucket's names and content puts below height (#223). 0 taken */
int hpplite_store_prune(HppliteStore *store, uint64_t height);
int hpplite_store_is_bucket(HppliteStore *store);
int hpplite_store_funder(HppliteStore *store, unsigned char out[20]);
/* funder= (#272): the bucket's funding wallet is another's (a platform's treasury, bound and permitted from its control plane): the
** node derives no funding key, binds nothing, gives no permit; a bucket bound elsewhere or a 402 is "waiting for the funder". */
void hpplite_store_set_funder(HppliteStore *store, const unsigned char funder[20]);
int hpplite_store_waiting_on_funder(HppliteStore *store);     /* 1 while the last refusal was the funder's to lift (#272; its daily cap, #288) */
/* The Bind(da, funder, issuedAt) digest in this store's domain, and the sequencer's half of it signed with the node's key (65 bytes):
** what a platform binding an app's bucket to its treasury needs from the app (SELECT hpplite_bind_signature(funder, issuedAt)). 0, or -1 */
int hpplite_store_bind_digest(HppliteStore *store, const unsigned char funder[20], uint64_t issuedAt, unsigned char digest[32]);
int hpplite_store_sign_bind(HppliteStore *store, const unsigned char funder[20], uint64_t issuedAt, unsigned char sig[65]);
/* The Cap(da, writesPerDay, bytesPerDay, issuedAt) digest in this store's domain (#274): the funder signs it to bound the bucket's day */
int hpplite_store_cap_digest(HppliteStore *store, uint64_t writesPerDay, uint64_t bytesPerDay, uint64_t issuedAt, unsigned char digest[32]);
int64_t hpplite_store_covered_until(HppliteStore *store);
const char *hpplite_store_bucket_state(HppliteStore *store);
int hpplite_store_statement(HppliteStore *store, uint8_t out[HPPLITE_STATEMENT_SIZE]);
/* A statement as an anchor carries it (hpplite-verify, #215): its fields, and 0 when its signature
** recovers to the signer it names, for this DA on this chain; -1 otherwise */
typedef struct HppliteStatement {
    unsigned char signer[20], payTo[20];
    uint64_t height, pruneHeight, coveredUntil, issuedAt;
} HppliteStatement;
int hpplite_store_statement_verify(const uint8_t *st, size_t n, uint64_t chainId, const unsigned char da[20], HppliteStatement *out);

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
/* What a put of nbytes costs at this provider, as its 402 will ask, in
** atomic units of its asset; 0 when it charges nothing (#172) */
uint64_t hpplite_store_quote(HppliteStore *store, size_t nbytes);
int hpplite_store_payment(HppliteStore *store, unsigned char asset[20], uint64_t *pChainId, uint64_t *pPerByte, uint64_t *pPerPin);

/* Seconds a payment authorization stays valid (default 120; a test chain whose clock ran ahead needs more) */
void hpplite_store_set_valid_for(HppliteStore *store, uint64_t seconds);

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
** and the signature apart (malloc'd bytes, or NULL). */
int hpplite_store_sign_batch(HppliteStore *store, const unsigned char da[20], uint64_t chainId, const unsigned char instance[32],
                             uint64_t height, const uint8_t *data, size_t len, uint8_t sig[65]);
int hpplite_store_put_named(HppliteStore *store, const char *zName, const unsigned char instance[32], const uint8_t *data, size_t len, const uint8_t sig[65]);
uint8_t *hpplite_store_get_named(HppliteStore *store, const char *zName, size_t *pLen, uint8_t sig[65], unsigned char instance[32]);
/* The wallet a named entry's signature recovers to (0), or -1 when it does not recover */
int hpplite_store_batch_signer(const unsigned char da[20], uint64_t chainId, const unsigned char instance[32], uint64_t height,
                               const uint8_t *data, size_t len, const uint8_t sig[65], unsigned char out[20]);
/* The store's published signer and signing domain (from its /status at open); 0 with them, -1 when the store named none */
int hpplite_store_signer(HppliteStore *store, unsigned char out[20]);
int hpplite_store_domain(HppliteStore *store, unsigned char payTo[20], uint64_t *pChainId);

/* Why the last put or get failed, for the node's choice between waiting and posting on chain */
#define HPPLITE_STORE_FAIL_NONE        0
#define HPPLITE_STORE_FAIL_UNREACHABLE 1   /* no answer: the chain can carry the bytes meanwhile */
#define HPPLITE_STORE_FAIL_CAP         2   /* the price is over HPPLITE_STORE_CAP: nothing signed, the batch waits */
#define HPPLITE_STORE_FAIL_OTHER       4
int hpplite_store_last_failure(HppliteStore *store);
long hpplite_store_take_status(HppliteStore *store);   /* the last request's HTTP status since last asked: -1 none, 0 no answer (#229) */

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_STORE_H */
