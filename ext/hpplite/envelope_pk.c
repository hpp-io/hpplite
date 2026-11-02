/*
** The public-key envelope (#15). See envelope_pk.h.
*/
#include "envelope_pk.h"
#include "keccak256.h"
#include <secp256k1.h>
#include <secp256k1_ecdh.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define HDR (HPPLITE_ENVELOPE_MAGIC_LEN + 3)                /* magic, scheme, encoding, n */
#define WRAP_PT 32
#define AD_MAX (2 + 8 + HPPLITE_PK_PUBKEY_LEN)

static secp256k1_context *g_ctx;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static void ctxInit(void) { g_ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE); }
static secp256k1_context *ctx(void) { pthread_once(&g_once, ctxInit); return g_ctx; }

void hpplite_pk_fingerprint(const uint8_t pubkey[33], uint8_t fp[4]) {
  uint8_t h[32];
  keccak256(pubkey, 33, h);
  memcpy(fp, h, 4);
}

int hpplite_pk_pubkey(const uint8_t privkey[32], uint8_t pubkey[33]) {
  secp256k1_pubkey pk;
  size_t n = 33;
  if (!secp256k1_ec_seckey_verify(ctx(), privkey)) return -1;
  if (!secp256k1_ec_pubkey_create(ctx(), &pk, privkey)) return -1;
  if (!secp256k1_ec_pubkey_serialize(ctx(), pubkey, &n, &pk, SECP256K1_EC_COMPRESSED)) return -1;
  return 0;
}

/* The wrapping key for (ephemeral, recipient): HKDF-SHA256 over the ECDH secret
** (libsecp256k1's default: sha256 of the compressed shared point) */
static int wrapKey(const uint8_t *theirPub, const uint8_t *myPriv, const uint8_t ephPub[33], uint8_t out[32]) {
  static const uint8_t salt[] = "hpplite";
  static const uint8_t info[] = "hpplite/envelope/v3/wrap";
  secp256k1_pubkey pk;
  uint8_t shared[32];
  if (!secp256k1_ec_pubkey_parse(ctx(), &pk, theirPub, 33)) return -1;
  if (!secp256k1_ecdh(ctx(), shared, &pk, myPriv, NULL, NULL)) return -1;
  {
    uint8_t ikm[32 + 33];
    memcpy(ikm, shared, 32);
    memcpy(ikm + 32, ephPub, 33);               /* the ephemeral key binds the secret to this envelope */
    hpplite_hkdf_sha256(salt, sizeof(salt) - 1, ikm, sizeof ikm, info, sizeof(info) - 1, out, 32);
    memset(ikm, 0, sizeof ikm);
  }
  memset(shared, 0, sizeof shared);
  return 0;
}

static size_t adPayload(uint8_t ad[AD_MAX], uint8_t scheme, uint8_t encoding, uint64_t height, const uint8_t ephPub[33]) {
  int i, k = 0;
  ad[k++] = scheme;
  ad[k++] = encoding;
  for (i = 0; i < 8; i++) ad[k++] = (uint8_t)(height >> (56 - 8 * i));
  memcpy(ad + k, ephPub, 33);
  return (size_t)k + 33;
}
static size_t adWrap(uint8_t ad[12], const uint8_t fp[4], uint64_t height) {
  int i;
  memcpy(ad, fp, 4);
  for (i = 0; i < 8; i++) ad[4 + i] = (uint8_t)(height >> (56 - 8 * i));
  return 12;
}

size_t hpplite_envelope_pk_overhead(const HppliteCipher *cipher, int nRecipients) {
  return HDR + HPPLITE_PK_PUBKEY_LEN + (size_t)nRecipients * (HPPLITE_PK_FP_LEN + WRAP_PT + cipher->tagLen)
         + cipher->nonceLen + cipher->tagLen;
}

uint8_t *hpplite_envelope_seal_pk(const HppliteCipher *cipher, const uint8_t dek[32],
                                  const uint8_t *aRecipients, int nRecipients,
                                  uint64_t height, uint8_t encoding,
                                  const uint8_t *payload, size_t payloadLen, size_t *pOutLen) {
  uint8_t ephPriv[32], ephPub[33], zeroNonce[12] = {0}, ad[AD_MAX];
  size_t wrapLen = HPPLITE_PK_FP_LEN + WRAP_PT + cipher->tagLen;
  size_t headLen, outLen, adLen;
  uint8_t *out, *p;
  int i;
  if (!cipher || !dek || !aRecipients || nRecipients < 1 || nRecipients > HPPLITE_PK_MAX_RECIPIENTS || cipher->nonceLen < 12) return NULL;
  do {
    if (hpplite_random_bytes(ephPriv, 32) != 0) return NULL;
  } while (hpplite_pk_pubkey(ephPriv, ephPub) != 0);
  headLen = HDR + HPPLITE_PK_PUBKEY_LEN + (size_t)nRecipients * wrapLen + cipher->nonceLen;
  outLen = headLen + payloadLen + cipher->tagLen;
  out = malloc(outLen);
  if (!out) { memset(ephPriv, 0, 32); return NULL; }
  p = out;
  memcpy(p, HPPLITE_ENVELOPE_MAGIC_V3, HPPLITE_ENVELOPE_MAGIC_LEN); p += HPPLITE_ENVELOPE_MAGIC_LEN;
  *p++ = cipher->id;
  *p++ = encoding;
  *p++ = (uint8_t)nRecipients;
  memcpy(p, ephPub, 33); p += 33;
  for (i = 0; i < nRecipients; i++) {
    const uint8_t *r = aRecipients + 33 * i;
    uint8_t wk[32], wad[12];
    hpplite_pk_fingerprint(r, p);
    if (wrapKey(r, ephPriv, ephPub, wk) != 0) { free(out); memset(ephPriv, 0, 32); return NULL; }
    adLen = adWrap(wad, p, height);
    /* the nonce is all zero: the wrapping key is unique to this envelope and recipient */
    if (cipher->seal(wk, zeroNonce, wad, adLen, dek, WRAP_PT, p + HPPLITE_PK_FP_LEN) != 0) { free(out); memset(ephPriv, 0, 32); return NULL; }
    memset(wk, 0, 32);
    p += wrapLen;
  }
  memset(ephPriv, 0, 32);
  if (hpplite_random_bytes(p, cipher->nonceLen) != 0) { free(out); return NULL; }
  adLen = adPayload(ad, cipher->id, encoding, height, ephPub);
  if (cipher->seal(dek, p, ad, adLen, payload, payloadLen, p + cipher->nonceLen) != 0) { free(out); return NULL; }
  if (pOutLen) *pOutLen = outLen;
  return out;
}

int hpplite_envelope_is_pk(const uint8_t *data, size_t len) {
  return data && len >= HDR && memcmp(data, HPPLITE_ENVELOPE_MAGIC_V3, HPPLITE_ENVELOPE_MAGIC_LEN) == 0;
}

int hpplite_envelope_pk_encoding(const uint8_t *data, size_t len) {
  return hpplite_envelope_is_pk(data, len) ? data[HPPLITE_ENVELOPE_MAGIC_LEN + 1] : -1;
}

int hpplite_envelope_pk_fingerprints(const uint8_t *data, size_t len, uint8_t *out, int max) {
  const HppliteCipher *cipher;
  int n, i;
  size_t wrapLen;
  if (!hpplite_envelope_is_pk(data, len)) return -1;
  cipher = hpplite_cipher_by_id(data[HPPLITE_ENVELOPE_MAGIC_LEN]);
  if (!cipher) return -1;
  n = data[HPPLITE_ENVELOPE_MAGIC_LEN + 2];
  wrapLen = HPPLITE_PK_FP_LEN + WRAP_PT + cipher->tagLen;
  if (len < HDR + 33 + (size_t)n * wrapLen) return -1;
  for (i = 0; i < n && i < max; i++) memcpy(out + 4 * i, data + HDR + 33 + (size_t)i * wrapLen, 4);
  return n;
}

uint8_t *hpplite_envelope_open_pk(const uint8_t privkey[32], uint64_t height,
                                  const uint8_t *data, size_t len, size_t *pOutLen,
                                  int *pForeign, uint8_t *pDek) {
  const HppliteCipher *cipher;
  uint8_t myPub[33], myFp[4], dek[32], wk[32], zeroNonce[12] = {0}, ad[AD_MAX], wad[12];
  const uint8_t *ephPub, *wraps, *nonce, *ct;
  size_t wrapLen, headLen, ctLen, adLen;
  int n, i, found = 0;
  uint8_t encoding;
  uint8_t *out;
  if (pForeign) *pForeign = 0;
  if (!hpplite_envelope_is_pk(data, len)) return NULL;
  cipher = hpplite_cipher_by_id(data[HPPLITE_ENVELOPE_MAGIC_LEN]);
  if (!cipher || cipher->nonceLen < 12) return NULL;
  encoding = data[HPPLITE_ENVELOPE_MAGIC_LEN + 1];
  n = data[HPPLITE_ENVELOPE_MAGIC_LEN + 2];
  wrapLen = HPPLITE_PK_FP_LEN + WRAP_PT + cipher->tagLen;
  headLen = HDR + 33 + (size_t)n * wrapLen + cipher->nonceLen;
  if (n < 1 || len < headLen + cipher->tagLen) return NULL;
  ephPub = data + HDR;
  wraps = ephPub + 33;
  nonce = wraps + (size_t)n * wrapLen;
  ct = nonce + cipher->nonceLen;
  ctLen = len - headLen;
  if (!privkey || hpplite_pk_pubkey(privkey, myPub) != 0) { if (pForeign) *pForeign = 1; return NULL; }
  hpplite_pk_fingerprint(myPub, myFp);
  for (i = 0; i < n && !found; i++) {
    const uint8_t *w = wraps + (size_t)i * wrapLen;
    if (memcmp(w, myFp, 4) != 0) continue;
    if (wrapKey(ephPub, privkey, ephPub, wk) != 0) continue;
    adLen = adWrap(wad, w, height);
    if (cipher->open(wk, zeroNonce, wad, adLen, w + HPPLITE_PK_FP_LEN, WRAP_PT + cipher->tagLen, dek) == 0) found = 1;
    memset(wk, 0, 32);
  }
  if (!found) { if (pForeign) *pForeign = 1; return NULL; }
  out = malloc(ctLen - cipher->tagLen + 1);
  if (!out) { memset(dek, 0, 32); return NULL; }
  adLen = adPayload(ad, cipher->id, encoding, height, ephPub);
  if (cipher->open(dek, nonce, ad, adLen, ct, ctLen, out) != 0) { free(out); memset(dek, 0, 32); return NULL; }
  out[ctLen - cipher->tagLen] = 0;
  if (pOutLen) *pOutLen = ctLen - cipher->tagLen;
  if (pDek) memcpy(pDek, dek, 32);
  memset(dek, 0, 32);
  return out;
}
