/*
** Ethereum keystore V3 (#17): the JSON file geth, cast and MetaMask exports
** write - the private key under AES-128-CTR with a key derived from a
** passphrase by scrypt or PBKDF2-SHA256, and a Keccak MAC over it.
** Vendored primitives, like cipher.c: nothing but sha256.c, cipher.c's
** HMAC and eth/keccak256.c underneath.
*/
#ifndef HPPLITE_KEYSTORE_H
#define HPPLITE_KEYSTORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The key of a keystore file, or of its JSON text: 0 and privkey; -1 with
** the reason in zErr (not a V3 keystore, an unsupported KDF or cipher, a
** wrong passphrase - the MAC does not match - a bad file) */
int hpplite_keystore_load(const char *zPath, const char *zPassword, unsigned char privkey[32], char *zErr, size_t nErr);
int hpplite_keystore_decrypt(const char *zJson, const char *zPassword, unsigned char privkey[32], char *zErr, size_t nErr);

/* The primitives, for their test vectors */
void hpplite_aes128_encrypt_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]);
void hpplite_aes128_ctr(const uint8_t key[16], const uint8_t iv[16], const uint8_t *in, size_t n, uint8_t *out);
void hpplite_pbkdf2_sha256(const uint8_t *pw, size_t npw, const uint8_t *salt, size_t nsalt, uint32_t c, uint8_t *out, size_t dklen);
/* RFC 7914; N a power of two up to 2^21, r up to 64, p up to 16: 0, or -1 (parameters, memory) */
int hpplite_scrypt(const uint8_t *pw, size_t npw, const uint8_t *salt, size_t nsalt, uint64_t N, uint32_t r, uint32_t p, uint8_t *out, size_t dklen);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_KEYSTORE_H */
