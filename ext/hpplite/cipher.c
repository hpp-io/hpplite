/*
** HPPLite batch encryption: ChaCha20-Poly1305 (RFC 8439), HMAC/HKDF-SHA256,
** envelope format. See cipher.h.
*/

#include "hpplite_sqlite.h"
#include "cipher.h"
#include "hpplite.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* ---------------------------------------------------------------------
** ChaCha20
*/
#define ROTL32(v, n) (((v) << (n)) | ((v) >> (32 - (n))))
#define QR(a, b, c, d) \
  a += b; d ^= a; d = ROTL32(d, 16); \
  c += d; b ^= c; b = ROTL32(b, 12); \
  a += b; d ^= a; d = ROTL32(d, 8);  \
  c += d; b ^= c; b = ROTL32(b, 7);

static uint32_t load32le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void store32le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void store64le(uint8_t *p, uint64_t v) {
  store32le(p, (uint32_t)v); store32le(p + 4, (uint32_t)(v >> 32));
}

static void chacha20_block(const uint32_t in[16], uint8_t out[64]) {
  uint32_t x[16];
  int i;
  memcpy(x, in, sizeof(x));
  for (i = 0; i < 10; i++) {
    QR(x[0], x[4], x[8],  x[12]) QR(x[1], x[5], x[9],  x[13])
    QR(x[2], x[6], x[10], x[14]) QR(x[3], x[7], x[11], x[15])
    QR(x[0], x[5], x[10], x[15]) QR(x[1], x[6], x[11], x[12])
    QR(x[2], x[7], x[8],  x[13]) QR(x[3], x[4], x[9],  x[14])
  }
  for (i = 0; i < 16; i++) store32le(out + 4 * i, x[i] + in[i]);
}

static void chacha20_init(uint32_t st[16], const uint8_t key[32], uint32_t counter, const uint8_t nonce[12]) {
  int i;
  st[0] = 0x61707865; st[1] = 0x3320646e; st[2] = 0x79622d32; st[3] = 0x6b206574;
  for (i = 0; i < 8; i++) st[4 + i] = load32le(key + 4 * i);
  st[12] = counter;
  st[13] = load32le(nonce); st[14] = load32le(nonce + 4); st[15] = load32le(nonce + 8);
}

static void chacha20_xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter,
                         const uint8_t *in, uint8_t *out, size_t len) {
  uint32_t st[16];
  uint8_t block[64];
  size_t i;
  chacha20_init(st, key, counter, nonce);
  while (len > 0) {
    size_t n = len < 64 ? len : 64;
    chacha20_block(st, block);
    st[12]++;
    for (i = 0; i < n; i++) out[i] = in[i] ^ block[i];
    in += n; out += n; len -= n;
  }
}

/* ---------------------------------------------------------------------
** Poly1305 (26-bit limbs, after poly1305-donna)
*/
typedef struct {
  uint32_t r[5];
  uint32_t h[5];
  uint32_t pad[4];
  size_t leftover;
  uint8_t buffer[16];
  uint8_t final;
} Poly1305;

static void poly1305_init(Poly1305 *st, const uint8_t key[32]) {
  uint32_t t0 = load32le(key), t1 = load32le(key + 4), t2 = load32le(key + 8), t3 = load32le(key + 12);
  st->r[0] = t0 & 0x3ffffff;
  st->r[1] = ((t0 >> 26) | (t1 << 6)) & 0x3ffff03;
  st->r[2] = ((t1 >> 20) | (t2 << 12)) & 0x3ffc0ff;
  st->r[3] = ((t2 >> 14) | (t3 << 18)) & 0x3f03fff;
  st->r[4] = (t3 >> 8) & 0x00fffff;
  memset(st->h, 0, sizeof(st->h));
  st->pad[0] = load32le(key + 16); st->pad[1] = load32le(key + 20);
  st->pad[2] = load32le(key + 24); st->pad[3] = load32le(key + 28);
  st->leftover = 0;
  st->final = 0;
}

static void poly1305_blocks(Poly1305 *st, const uint8_t *m, size_t bytes) {
  const uint32_t hibit = st->final ? 0 : (1u << 24);
  uint32_t r0 = st->r[0], r1 = st->r[1], r2 = st->r[2], r3 = st->r[3], r4 = st->r[4];
  uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
  uint32_t h0 = st->h[0], h1 = st->h[1], h2 = st->h[2], h3 = st->h[3], h4 = st->h[4];
  while (bytes >= 16) {
    uint64_t d0, d1, d2, d3, d4;
    uint32_t c;
    h0 += (load32le(m + 0)) & 0x3ffffff;
    h1 += (load32le(m + 3) >> 2) & 0x3ffffff;
    h2 += (load32le(m + 6) >> 4) & 0x3ffffff;
    h3 += (load32le(m + 9) >> 6) & 0x3ffffff;
    h4 += (load32le(m + 12) >> 8) | hibit;
    d0 = (uint64_t)h0 * r0 + (uint64_t)h1 * s4 + (uint64_t)h2 * s3 + (uint64_t)h3 * s2 + (uint64_t)h4 * s1;
    d1 = (uint64_t)h0 * r1 + (uint64_t)h1 * r0 + (uint64_t)h2 * s4 + (uint64_t)h3 * s3 + (uint64_t)h4 * s2;
    d2 = (uint64_t)h0 * r2 + (uint64_t)h1 * r1 + (uint64_t)h2 * r0 + (uint64_t)h3 * s4 + (uint64_t)h4 * s3;
    d3 = (uint64_t)h0 * r3 + (uint64_t)h1 * r2 + (uint64_t)h2 * r1 + (uint64_t)h3 * r0 + (uint64_t)h4 * s4;
    d4 = (uint64_t)h0 * r4 + (uint64_t)h1 * r3 + (uint64_t)h2 * r2 + (uint64_t)h3 * r1 + (uint64_t)h4 * r0;
    c = (uint32_t)(d0 >> 26); h0 = (uint32_t)d0 & 0x3ffffff;
    d1 += c; c = (uint32_t)(d1 >> 26); h1 = (uint32_t)d1 & 0x3ffffff;
    d2 += c; c = (uint32_t)(d2 >> 26); h2 = (uint32_t)d2 & 0x3ffffff;
    d3 += c; c = (uint32_t)(d3 >> 26); h3 = (uint32_t)d3 & 0x3ffffff;
    d4 += c; c = (uint32_t)(d4 >> 26); h4 = (uint32_t)d4 & 0x3ffffff;
    h0 += c * 5; c = h0 >> 26; h0 &= 0x3ffffff;
    h1 += c;
    m += 16; bytes -= 16;
  }
  st->h[0] = h0; st->h[1] = h1; st->h[2] = h2; st->h[3] = h3; st->h[4] = h4;
}

static void poly1305_update(Poly1305 *st, const uint8_t *m, size_t bytes) {
  size_t i;
  if (st->leftover) {
    size_t want = 16 - st->leftover;
    if (want > bytes) want = bytes;
    for (i = 0; i < want; i++) st->buffer[st->leftover + i] = m[i];
    bytes -= want; m += want; st->leftover += want;
    if (st->leftover < 16) return;
    poly1305_blocks(st, st->buffer, 16);
    st->leftover = 0;
  }
  if (bytes >= 16) {
    size_t want = bytes & ~(size_t)15;
    poly1305_blocks(st, m, want);
    m += want; bytes -= want;
  }
  if (bytes) {
    for (i = 0; i < bytes; i++) st->buffer[st->leftover + i] = m[i];
    st->leftover += bytes;
  }
}

static void poly1305_finish(Poly1305 *st, uint8_t mac[16]) {
  uint32_t h0, h1, h2, h3, h4, c, g0, g1, g2, g3, g4, mask;
  uint64_t f;
  if (st->leftover) {
    size_t i = st->leftover;
    st->buffer[i++] = 1;
    for (; i < 16; i++) st->buffer[i] = 0;
    st->final = 1;
    poly1305_blocks(st, st->buffer, 16);
  }
  h0 = st->h[0]; h1 = st->h[1]; h2 = st->h[2]; h3 = st->h[3]; h4 = st->h[4];
  c = h1 >> 26; h1 &= 0x3ffffff; h2 += c;
  c = h2 >> 26; h2 &= 0x3ffffff; h3 += c;
  c = h3 >> 26; h3 &= 0x3ffffff; h4 += c;
  c = h4 >> 26; h4 &= 0x3ffffff; h0 += c * 5;
  c = h0 >> 26; h0 &= 0x3ffffff; h1 += c;
  /* compute h + -p */
  g0 = h0 + 5; c = g0 >> 26; g0 &= 0x3ffffff;
  g1 = h1 + c; c = g1 >> 26; g1 &= 0x3ffffff;
  g2 = h2 + c; c = g2 >> 26; g2 &= 0x3ffffff;
  g3 = h3 + c; c = g3 >> 26; g3 &= 0x3ffffff;
  g4 = h4 + c - (1u << 26);
  /* select h if h < p, or h + -p if h >= p */
  mask = (g4 >> 31) - 1;
  g0 &= mask; g1 &= mask; g2 &= mask; g3 &= mask; g4 &= mask;
  mask = ~mask;
  h0 = (h0 & mask) | g0; h1 = (h1 & mask) | g1; h2 = (h2 & mask) | g2;
  h3 = (h3 & mask) | g3; h4 = (h4 & mask) | g4;
  /* h = h % (2^128) */
  h0 = (h0 | (h1 << 26)) & 0xffffffff;
  h1 = ((h1 >> 6) | (h2 << 20)) & 0xffffffff;
  h2 = ((h2 >> 12) | (h3 << 14)) & 0xffffffff;
  h3 = ((h3 >> 18) | (h4 << 8)) & 0xffffffff;
  /* mac = (h + pad) % (2^128) */
  f = (uint64_t)h0 + st->pad[0];             h0 = (uint32_t)f;
  f = (uint64_t)h1 + st->pad[1] + (f >> 32); h1 = (uint32_t)f;
  f = (uint64_t)h2 + st->pad[2] + (f >> 32); h2 = (uint32_t)f;
  f = (uint64_t)h3 + st->pad[3] + (f >> 32); h3 = (uint32_t)f;
  store32le(mac, h0); store32le(mac + 4, h1); store32le(mac + 8, h2); store32le(mac + 12, h3);
  memset(st, 0, sizeof(*st));
}

static int ct_equal(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t d = 0;
  size_t i;
  for (i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

/* ---------------------------------------------------------------------
** AEAD_CHACHA20_POLY1305 (RFC 8439 section 2.8)
*/
static void aead_tag(const uint8_t polyKey[32], const uint8_t *ad, size_t adLen,
                     const uint8_t *ct, size_t ctLen, uint8_t tag[16]) {
  static const uint8_t zero[16] = {0};
  Poly1305 st;
  uint8_t lens[16];
  poly1305_init(&st, polyKey);
  poly1305_update(&st, ad, adLen);
  if (adLen % 16) poly1305_update(&st, zero, 16 - (adLen % 16));
  poly1305_update(&st, ct, ctLen);
  if (ctLen % 16) poly1305_update(&st, zero, 16 - (ctLen % 16));
  store64le(lens, (uint64_t)adLen);
  store64le(lens + 8, (uint64_t)ctLen);
  poly1305_update(&st, lens, 16);
  poly1305_finish(&st, tag);
}

static void aead_poly_key(const uint8_t key[32], const uint8_t nonce[12], uint8_t polyKey[32]) {
  uint32_t st[16];
  uint8_t block[64];
  chacha20_init(st, key, 0, nonce);
  chacha20_block(st, block);
  memcpy(polyKey, block, 32);
  memset(block, 0, sizeof(block));
}

static int chacha_seal(const uint8_t key[32], const uint8_t *nonce,
                       const uint8_t *ad, size_t adLen,
                       const uint8_t *pt, size_t ptLen, uint8_t *out) {
  uint8_t polyKey[32];
  aead_poly_key(key, nonce, polyKey);
  chacha20_xor(key, nonce, 1, pt, out, ptLen);
  aead_tag(polyKey, ad, adLen, out, ptLen, out + ptLen);
  memset(polyKey, 0, sizeof(polyKey));
  return 0;
}

static int chacha_open(const uint8_t key[32], const uint8_t *nonce,
                       const uint8_t *ad, size_t adLen,
                       const uint8_t *ct, size_t ctLen, uint8_t *out) {
  uint8_t polyKey[32], tag[16];
  if (ctLen < 16) return -1;
  aead_poly_key(key, nonce, polyKey);
  aead_tag(polyKey, ad, adLen, ct, ctLen - 16, tag);
  memset(polyKey, 0, sizeof(polyKey));
  if (!ct_equal(tag, ct + ctLen - 16, 16)) return -1;
  chacha20_xor(key, nonce, 1, ct, out, ctLen - 16);
  return 0;
}

const HppliteCipher HPPLITE_CIPHER_CHACHA20POLY1305 = {
  "chacha20poly1305", 1, 12, 16, chacha_seal, chacha_open
};

static const HppliteCipher *aCiphers[] = { &HPPLITE_CIPHER_CHACHA20POLY1305, NULL };

const HppliteCipher *hpplite_cipher_default(void) { return &HPPLITE_CIPHER_CHACHA20POLY1305; }

const HppliteCipher *hpplite_cipher_by_id(uint8_t id) {
  int i;
  for (i = 0; aCiphers[i]; i++) if (aCiphers[i]->id == id) return aCiphers[i];
  return NULL;
}

const HppliteCipher *hpplite_cipher_by_name(const char *name) {
  int i;
  if (!name) return NULL;
  for (i = 0; aCiphers[i]; i++) if (strcmp(aCiphers[i]->name, name) == 0) return aCiphers[i];
  return NULL;
}

/* ---------------------------------------------------------------------
** HMAC-SHA256 / HKDF-SHA256 (RFC 2104 / RFC 5869) over hpplite_sha256
*/
void hpplite_hmac_sha256(const uint8_t *key, size_t keyLen, const uint8_t *msg, size_t msgLen, uint8_t out[32]) {
  uint8_t k[64], ipad[64], opad[64], inner[32];
  uint8_t *buf;
  size_t i;
  memset(k, 0, sizeof(k));
  if (keyLen > 64) hpplite_sha256(key, keyLen, k); else memcpy(k, key, keyLen);
  for (i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
  buf = malloc(64 + (msgLen > 32 ? msgLen : 32));
  if (!buf) { memset(out, 0, 32); return; }
  memcpy(buf, ipad, 64); memcpy(buf + 64, msg, msgLen);
  hpplite_sha256(buf, 64 + msgLen, inner);
  memcpy(buf, opad, 64); memcpy(buf + 64, inner, 32);
  hpplite_sha256(buf, 64 + 32, out);
  free(buf);
  memset(k, 0, sizeof(k)); memset(ipad, 0, sizeof(ipad)); memset(opad, 0, sizeof(opad));
}

void hpplite_hkdf_sha256(const uint8_t *salt, size_t saltLen, const uint8_t *ikm, size_t ikmLen,
                         const uint8_t *info, size_t infoLen, uint8_t *okm, size_t okmLen) {
  uint8_t prk[32], t[32];
  uint8_t *msg;
  size_t tLen = 0, done = 0;
  uint8_t counter = 1;
  static const uint8_t zeroSalt[32] = {0};
  if (!salt) { salt = zeroSalt; saltLen = 32; }
  hpplite_hmac_sha256(salt, saltLen, ikm, ikmLen, prk);
  msg = malloc(32 + infoLen + 1);
  if (!msg) return;
  while (done < okmLen) {
    size_t n;
    memcpy(msg, t, tLen);
    memcpy(msg + tLen, info, infoLen);
    msg[tLen + infoLen] = counter++;
    hpplite_hmac_sha256(prk, 32, msg, tLen + infoLen + 1, t);
    tLen = 32;
    n = okmLen - done < 32 ? okmLen - done : 32;
    memcpy(okm + done, t, n);
    done += n;
  }
  free(msg);
  memset(prk, 0, sizeof(prk)); memset(t, 0, sizeof(t));
}

void hpplite_cipher_derive_key(const uint8_t privkey[32], uint8_t key[32]) {
  static const uint8_t salt[] = "hpplite";
  static const uint8_t info[] = "hpplite/batch-encryption/v1";
  hpplite_hkdf_sha256(salt, sizeof(salt) - 1, privkey, 32, info, sizeof(info) - 1, key, 32);
}

int hpplite_random_bytes(uint8_t *out, size_t len) {
  FILE *f = fopen("/dev/urandom", "rb");
  size_t n = 0;
  if (f) { n = fread(out, 1, len, f); fclose(f); }
  return n == len ? 0 : -1;
}

/* ---------------------------------------------------------------------
** Envelope
*/
/* v2 associated data: scheme | encoding | height; v1 had no encoding byte */
static size_t envelope_ad(uint8_t ad[10], uint8_t scheme, uint8_t encoding, uint64_t height) {
  int i, k = 0;
  ad[k++] = scheme;
  ad[k++] = encoding;
  for (i = 0; i < 8; i++) ad[k++] = (uint8_t)(height >> (56 - 8 * i));
  return (size_t)k;
}

size_t hpplite_envelope_overhead(const HppliteCipher *cipher) {
  return HPPLITE_ENVELOPE_HEADER_LEN + cipher->nonceLen + cipher->tagLen;
}

uint8_t *hpplite_envelope_seal(const HppliteCipher *cipher, const uint8_t key[32],
                               uint64_t height, uint8_t encoding,
                               const uint8_t *payload, size_t payloadLen,
                               size_t *pOutLen) {
  size_t headLen = HPPLITE_ENVELOPE_HEADER_LEN + cipher->nonceLen;
  size_t outLen = headLen + payloadLen + cipher->tagLen;
  uint8_t *out = malloc(outLen);
  uint8_t ad[10];
  size_t adLen;
  if (!out) return NULL;
  memcpy(out, HPPLITE_ENVELOPE_MAGIC, HPPLITE_ENVELOPE_MAGIC_LEN);
  out[HPPLITE_ENVELOPE_MAGIC_LEN] = cipher->id;
  out[HPPLITE_ENVELOPE_MAGIC_LEN + 1] = encoding;
  if (hpplite_random_bytes(out + HPPLITE_ENVELOPE_HEADER_LEN, cipher->nonceLen) != 0) {
    free(out);
    return NULL;
  }
  adLen = envelope_ad(ad, cipher->id, encoding, height);
  if (cipher->seal(key, out + HPPLITE_ENVELOPE_HEADER_LEN, ad, adLen,
                   payload, payloadLen, out + headLen) != 0) {
    free(out);
    return NULL;
  }
  if (pOutLen) *pOutLen = outLen;
  return out;
}

/* "HPE2" and at least the scheme byte; nothing else is an envelope (#249: the pre-release "HPE1" is not read) */
static int envelope_is_sealed(const uint8_t *data, size_t len) {
  return data && len >= HPPLITE_ENVELOPE_MAGIC_LEN + 1 && memcmp(data, HPPLITE_ENVELOPE_MAGIC, HPPLITE_ENVELOPE_MAGIC_LEN) == 0;
}

int hpplite_envelope_is_sealed(const uint8_t *data, size_t len) {
  return envelope_is_sealed(data, len);
}

int hpplite_envelope_encoding(const uint8_t *data, size_t len) {
  if (!envelope_is_sealed(data, len)) return -1;
  return len > HPPLITE_ENVELOPE_MAGIC_LEN + 1 ? data[HPPLITE_ENVELOPE_MAGIC_LEN + 1] : -1;
}

uint8_t *hpplite_envelope_open(const uint8_t key[32], uint64_t height,
                               const uint8_t *data, size_t len, size_t *pOutLen,
                               const HppliteCipher **pScheme) {
  const HppliteCipher *cipher;
  size_t headLen, ctLen, adLen;
  uint8_t ad[10], encoding;
  uint8_t *out;
  if (!envelope_is_sealed(data, len)) return NULL;
  cipher = hpplite_cipher_by_id(data[HPPLITE_ENVELOPE_MAGIC_LEN]);
  if (pScheme) *pScheme = cipher;
  if (!cipher) return NULL;
  headLen = HPPLITE_ENVELOPE_HEADER_LEN + cipher->nonceLen;
  if (len < headLen + cipher->tagLen) return NULL;   /* before touching any header byte */
  encoding = data[HPPLITE_ENVELOPE_MAGIC_LEN + 1];
  ctLen = len - headLen;
  out = malloc(ctLen - cipher->tagLen + 1);
  if (!out) return NULL;
  adLen = envelope_ad(ad, cipher->id, encoding, height);
  if (cipher->open(key, data + HPPLITE_ENVELOPE_HEADER_LEN, ad, adLen, data + headLen, ctLen, out) != 0) {
    free(out);
    return NULL;
  }
  out[ctLen - cipher->tagLen] = 0;
  if (pOutLen) *pOutLen = ctLen - cipher->tagLen;
  return out;
}
