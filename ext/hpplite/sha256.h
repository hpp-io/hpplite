/*
** HPPLite - SHA-256 (internal). hpplite_sha256() in hpplite.h is the one-shot form.
*/
#ifndef HPPLITE_SHA256_H
#define HPPLITE_SHA256_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
  uint8_t data[64];
  uint32_t datalen;
  uint64_t bitlen;
  uint32_t state[8];
} SHA256_CTX;

void hpplite_sha256_init(SHA256_CTX *ctx);
void hpplite_sha256_update(SHA256_CTX *ctx, const uint8_t *data, size_t len);
void hpplite_sha256_final(SHA256_CTX *ctx, uint8_t *hash);
/* one shot over prefix || data, the platform's implementation where it has hardware support (#146) */
void hpplite_sha256_prefixed(const uint8_t *prefix, size_t nPrefix, const uint8_t *data, size_t n, uint8_t out[32]);

#endif /* HPPLITE_SHA256_H */
