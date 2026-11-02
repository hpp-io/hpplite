/*
** Ethereum keystore V3 (#17). See keystore.h.
*/
#include "keystore.h"
#include "cipher.h"
#include "keccak256.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* === AES-128, encryption only (CTR needs no decryption) === */

static const uint8_t sbox[256] = {
  0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
  0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
  0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
  0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
  0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
  0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
  0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
  0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
  0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
  0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
  0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
  0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
  0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
  0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
  0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
  0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16 };

static uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0)); }

static void aes128_expand(const uint8_t key[16], uint8_t rk[176]) {
  uint8_t rcon = 1;
  int i;
  memcpy(rk, key, 16);
  for (i = 16; i < 176; i += 4) {
    uint8_t t[4];
    memcpy(t, rk + i - 4, 4);
    if (i % 16 == 0) {
      uint8_t u = t[0];
      t[0] = sbox[t[1]] ^ rcon; t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[u];
      rcon = xtime(rcon);
    }
    rk[i] = rk[i - 16] ^ t[0]; rk[i + 1] = rk[i - 15] ^ t[1]; rk[i + 2] = rk[i - 14] ^ t[2]; rk[i + 3] = rk[i - 13] ^ t[3];
  }
}

void hpplite_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  uint8_t rk[176], s[16];
  int round, i;
  aes128_expand(key, rk);
  for (i = 0; i < 16; i++) s[i] = in[i] ^ rk[i];
  for (round = 1; round <= 10; round++) {
    uint8_t t[16];
    for (i = 0; i < 16; i++) s[i] = sbox[s[i]];                       /* SubBytes */
    for (i = 0; i < 16; i++) t[i] = s[(i + 4 * (i % 4)) % 16];         /* ShiftRows: row r rotates by r (column-major state) */
    if (round < 10) {                                                 /* MixColumns */
      for (i = 0; i < 4; i++) {
        uint8_t *c = t + 4 * i, a0 = c[0], a1 = c[1], a2 = c[2], a3 = c[3];
        c[0] = (uint8_t)(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        c[1] = (uint8_t)(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        c[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        c[3] = (uint8_t)((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
      }
    }
    for (i = 0; i < 16; i++) s[i] = t[i] ^ rk[16 * round + i];       /* AddRoundKey */
  }
  memcpy(out, s, 16);
  memset(rk, 0, sizeof rk);
}

void hpplite_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out) {
  uint8_t ctr[16], ks[16];
  size_t i = 0;
  memcpy(ctr, iv, 16);
  while (i < n) {
    size_t j, k = n - i < 16 ? n - i : 16;
    hpplite_aes128_encrypt_block(key, ctr, ks);
    for (j = 0; j < k; j++) out[i + j] = in[i + j] ^ ks[j];
    i += k;
    for (j = 16; j-- > 0;) if (++ctr[j]) break;                       /* big-endian counter over the whole block */
  }
  memset(ks, 0, sizeof ks);
}

/* === PBKDF2-HMAC-SHA256 (RFC 8018) === */

void hpplite_pbkdf2_sha256(const uint8_t *pw, size_t npw, const uint8_t *salt, size_t nsalt, uint32_t c, uint8_t *out, size_t dklen) {
  uint8_t *buf = malloc(nsalt + 4);
  uint32_t block = 1;
  size_t done = 0;
  if (!buf) { memset(out, 0, dklen); return; }
  memcpy(buf, salt, nsalt);
  while (done < dklen) {
    uint8_t u[32], t[32];
    uint32_t i;
    size_t k = dklen - done < 32 ? dklen - done : 32;
    buf[nsalt] = (uint8_t)(block >> 24); buf[nsalt + 1] = (uint8_t)(block >> 16); buf[nsalt + 2] = (uint8_t)(block >> 8); buf[nsalt + 3] = (uint8_t)block;
    hpplite_hmac_sha256(pw, npw, buf, nsalt + 4, u);
    memcpy(t, u, 32);
    for (i = 1; i < c; i++) {
      int j;
      hpplite_hmac_sha256(pw, npw, u, 32, u);
      for (j = 0; j < 32; j++) t[j] ^= u[j];
    }
    memcpy(out + done, t, k);
    done += k;
    block++;
  }
  free(buf);
}

/* === scrypt (RFC 7914) === */

#define R32(a, b) (((a) << (b)) | ((a) >> (32 - (b))))
static void salsa20_8(uint32_t b[16]) {
  uint32_t x[16];
  int i;
  memcpy(x, b, sizeof x);
  for (i = 0; i < 8; i += 2) {
    x[ 4] ^= R32(x[ 0] + x[12], 7); x[ 8] ^= R32(x[ 4] + x[ 0], 9); x[12] ^= R32(x[ 8] + x[ 4], 13); x[ 0] ^= R32(x[12] + x[ 8], 18);
    x[ 9] ^= R32(x[ 5] + x[ 1], 7); x[13] ^= R32(x[ 9] + x[ 5], 9); x[ 1] ^= R32(x[13] + x[ 9], 13); x[ 5] ^= R32(x[ 1] + x[13], 18);
    x[14] ^= R32(x[10] + x[ 6], 7); x[ 2] ^= R32(x[14] + x[10], 9); x[ 6] ^= R32(x[ 2] + x[14], 13); x[10] ^= R32(x[ 6] + x[ 2], 18);
    x[ 3] ^= R32(x[15] + x[11], 7); x[ 7] ^= R32(x[ 3] + x[15], 9); x[11] ^= R32(x[ 7] + x[ 3], 13); x[15] ^= R32(x[11] + x[ 7], 18);
    x[ 1] ^= R32(x[ 0] + x[ 3], 7); x[ 2] ^= R32(x[ 1] + x[ 0], 9); x[ 3] ^= R32(x[ 2] + x[ 1], 13); x[ 0] ^= R32(x[ 3] + x[ 2], 18);
    x[ 6] ^= R32(x[ 5] + x[ 4], 7); x[ 7] ^= R32(x[ 6] + x[ 5], 9); x[ 4] ^= R32(x[ 7] + x[ 6], 13); x[ 5] ^= R32(x[ 4] + x[ 7], 18);
    x[11] ^= R32(x[10] + x[ 9], 7); x[ 8] ^= R32(x[11] + x[10], 9); x[ 9] ^= R32(x[ 8] + x[11], 13); x[10] ^= R32(x[ 9] + x[ 8], 18);
    x[12] ^= R32(x[15] + x[14], 7); x[13] ^= R32(x[12] + x[15], 9); x[14] ^= R32(x[13] + x[12], 13); x[15] ^= R32(x[14] + x[13], 18);
  }
  for (i = 0; i < 16; i++) b[i] += x[i];
}

/* B and Y are 2r blocks of 16 words; words little-endian in memory throughout */
static void blockmix(uint32_t *B, uint32_t *Y, uint32_t r) {
  uint32_t X[16];
  uint32_t i, j;
  memcpy(X, B + (2 * r - 1) * 16, 64);
  for (i = 0; i < 2 * r; i++) {
    for (j = 0; j < 16; j++) X[j] ^= B[i * 16 + j];
    salsa20_8(X);
    memcpy(Y + i * 16, X, 64);
  }
  for (i = 0; i < r; i++) memcpy(B + i * 16, Y + (2 * i) * 16, 64);
  for (i = 0; i < r; i++) memcpy(B + (r + i) * 16, Y + (2 * i + 1) * 16, 64);
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void romix(uint8_t *block, uint32_t r, uint64_t N, uint32_t *V, uint32_t *X, uint32_t *Y) {
  uint32_t words = 32 * r;
  uint64_t i;
  uint32_t k;
  for (k = 0; k < words; k++) X[k] = le32(block + 4 * k);
  for (i = 0; i < N; i++) { memcpy(V + i * words, X, words * 4); blockmix(X, Y, r); }
  for (i = 0; i < N; i++) {
    uint64_t j = X[(2 * r - 1) * 16] & (N - 1);                       /* Integerify: the last block's first word, N a power of two */
    for (k = 0; k < words; k++) X[k] ^= V[j * words + k];
    blockmix(X, Y, r);
  }
  for (k = 0; k < words; k++) put32(block + 4 * k, X[k]);
}

int hpplite_scrypt(const uint8_t *pw, size_t npw, const uint8_t *salt, size_t nsalt, uint64_t N, uint32_t r, uint32_t p, uint8_t *out, size_t dklen) {
  uint8_t *B;
  uint32_t *V, *X, *Y;
  uint32_t i;
  if (N < 2 || (N & (N - 1)) || N > ((uint64_t)1 << 21) || r == 0 || r > 64 || p == 0 || p > 16) return -1;
  B = malloc((size_t)p * 128 * r);
  V = malloc((size_t)N * 128 * r);
  X = malloc(128 * r);
  Y = malloc(128 * r);
  if (!B || !V || !X || !Y) { free(B); free(V); free(X); free(Y); return -1; }
  hpplite_pbkdf2_sha256(pw, npw, salt, nsalt, 1, B, (size_t)p * 128 * r);
  for (i = 0; i < p; i++) romix(B + (size_t)i * 128 * r, r, N, V, X, Y);
  hpplite_pbkdf2_sha256(pw, npw, B, (size_t)p * 128 * r, 1, out, dklen);
  memset(B, 0, (size_t)p * 128 * r);
  free(B); free(V); free(X); free(Y);
  return 0;
}

/* === the keystore === */

static int hexOf(const cJSON *j, uint8_t *out, size_t n) {
  size_t i;
  const char *z;
  if (!j || !cJSON_IsString(j)) return -1;
  z = j->valuestring;
  if (z[0] == '0' && (z[1] == 'x' || z[1] == 'X')) z += 2;
  if (strlen(z) != n * 2) return -1;
  for (i = 0; i < n; i++) {
    unsigned v;
    if (sscanf(z + 2 * i, "%2x", &v) != 1) return -1;
    out[i] = (uint8_t)v;
  }
  return 0;
}
static uint64_t numOf(const cJSON *j) { return (j && cJSON_IsNumber(j)) ? (uint64_t)j->valuedouble : 0; }
static int fail(char *zErr, size_t nErr, const char *z) { if (zErr && nErr) snprintf(zErr, nErr, "%s", z); return -1; }

int hpplite_keystore_decrypt(const char *zJson, const char *zPassword, unsigned char privkey[32], char *zErr, size_t nErr) {
  cJSON *root = zJson ? cJSON_Parse(zJson) : NULL;
  cJSON *crypto, *kdf, *kp, *cipher, *cp, *mac;
  uint8_t salt[64], iv[16], ct[32], macWant[32], macGot[32], derived[32], tmp[48];
  size_t nsalt;
  int rc = -1;
  if (!zPassword) { cJSON_Delete(root); return fail(zErr, nErr, "no passphrase (HPPLITE_KEYSTORE_PASSWORD)"); }
  if (!root) return fail(zErr, nErr, "not JSON");
  if (numOf(cJSON_GetObjectItem(root, "version")) != 3) { cJSON_Delete(root); return fail(zErr, nErr, "not a version 3 keystore"); }
  crypto = cJSON_GetObjectItem(root, "crypto");
  if (!crypto) crypto = cJSON_GetObjectItem(root, "Crypto");
  kdf = crypto ? cJSON_GetObjectItem(crypto, "kdf") : NULL;
  kp = crypto ? cJSON_GetObjectItem(crypto, "kdfparams") : NULL;
  cipher = crypto ? cJSON_GetObjectItem(crypto, "cipher") : NULL;
  cp = crypto ? cJSON_GetObjectItem(crypto, "cipherparams") : NULL;
  mac = crypto ? cJSON_GetObjectItem(crypto, "mac") : NULL;
  if (!kdf || !cJSON_IsString(kdf) || !kp || !cipher || !cJSON_IsString(cipher) || !cp) { cJSON_Delete(root); return fail(zErr, nErr, "crypto section incomplete"); }
  if (strcmp(cipher->valuestring, "aes-128-ctr") != 0) { cJSON_Delete(root); return fail(zErr, nErr, "cipher is not aes-128-ctr"); }
  if (hexOf(cJSON_GetObjectItem(cp, "iv"), iv, 16) != 0) { cJSON_Delete(root); return fail(zErr, nErr, "cipherparams.iv is not 16 bytes of hex"); }
  if (hexOf(cJSON_GetObjectItem(crypto, "ciphertext"), ct, 32) != 0) { cJSON_Delete(root); return fail(zErr, nErr, "ciphertext is not 32 bytes of hex"); }
  if (hexOf(mac, macWant, 32) != 0) { cJSON_Delete(root); return fail(zErr, nErr, "mac is not 32 bytes of hex"); }
  {
    const cJSON *js = cJSON_GetObjectItem(kp, "salt");
    const char *z = (js && cJSON_IsString(js)) ? js->valuestring : NULL;
    if (z && z[0] == '0' && (z[1] == 'x' || z[1] == 'X')) z += 2;
    nsalt = z ? strlen(z) / 2 : 0;
    if (!z || nsalt == 0 || nsalt > sizeof salt || hexOf(js, salt, nsalt) != 0) { cJSON_Delete(root); return fail(zErr, nErr, "kdfparams.salt is not hex"); }
  }
  if (numOf(cJSON_GetObjectItem(kp, "dklen")) != 32) { cJSON_Delete(root); return fail(zErr, nErr, "kdfparams.dklen is not 32"); }
  if (strcmp(kdf->valuestring, "scrypt") == 0) {
    uint64_t N = numOf(cJSON_GetObjectItem(kp, "n"));
    uint32_t r = (uint32_t)numOf(cJSON_GetObjectItem(kp, "r")), p = (uint32_t)numOf(cJSON_GetObjectItem(kp, "p"));
    if (hpplite_scrypt((const uint8_t *)zPassword, strlen(zPassword), salt, nsalt, N, r, p, derived, 32) != 0) {
      cJSON_Delete(root); return fail(zErr, nErr, "scrypt parameters out of range (n a power of two up to 2^21, r up to 64, p up to 16)");
    }
  } else if (strcmp(kdf->valuestring, "pbkdf2") == 0) {
    const cJSON *prf = cJSON_GetObjectItem(kp, "prf");
    uint64_t c = numOf(cJSON_GetObjectItem(kp, "c"));
    if (!prf || !cJSON_IsString(prf) || strcmp(prf->valuestring, "hmac-sha256") != 0) { cJSON_Delete(root); return fail(zErr, nErr, "pbkdf2 prf is not hmac-sha256"); }
    if (c == 0 || c > 100000000) { cJSON_Delete(root); return fail(zErr, nErr, "pbkdf2 c out of range"); }
    hpplite_pbkdf2_sha256((const uint8_t *)zPassword, strlen(zPassword), salt, nsalt, (uint32_t)c, derived, 32);
  } else {
    cJSON_Delete(root);
    return fail(zErr, nErr, "kdf is neither scrypt nor pbkdf2");
  }
  cJSON_Delete(root);
  memcpy(tmp, derived + 16, 16);
  memcpy(tmp + 16, ct, 32);
  keccak256(tmp, 48, macGot);
  if (memcmp(macGot, macWant, 32) != 0) {
    rc = fail(zErr, nErr, "wrong passphrase (the MAC does not match)");
  } else {
    hpplite_aes128_ctr(derived, iv, ct, 32, privkey);
    rc = 0;
  }
  memset(derived, 0, sizeof derived);
  memset(tmp, 0, sizeof tmp);
  return rc;
}

int hpplite_keystore_load(const char *zPath, const char *zPassword, unsigned char privkey[32], char *zErr, size_t nErr) {
  FILE *f = zPath ? fopen(zPath, "rb") : NULL;
  char *buf;
  size_t n = 0, cap = 1 << 16;
  int rc;
  if (!f) return fail(zErr, nErr, "cannot read the file");
  buf = malloc(cap);
  if (!buf) { fclose(f); return fail(zErr, nErr, "out of memory"); }
  n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  rc = hpplite_keystore_decrypt(buf, zPassword, privkey, zErr, nErr);
  free(buf);
  return rc;
}
