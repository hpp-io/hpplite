/*
** HPPLite batch compression (#12): zlib's deflate, level 6. See compress.h
** for the posted form.
*/
#include "compress.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

uint8_t *hpplite_deflate(const uint8_t *in, size_t n, size_t *pOut) {
    uLongf bound;
    uint8_t *out;
    if (pOut) *pOut = 0;
    if (!in || n == 0 || n > HPPLITE_INFLATE_MAX) return NULL;
    bound = compressBound((uLong)n);
    out = malloc(4 + bound);
    if (!out) return NULL;
    out[0] = (uint8_t)(n >> 24); out[1] = (uint8_t)(n >> 16); out[2] = (uint8_t)(n >> 8); out[3] = (uint8_t)n;
    if (compress2(out + 4, &bound, in, (uLong)n, 6) != Z_OK) { free(out); return NULL; }
    if (pOut) *pOut = 4 + (size_t)bound;
    return out;
}

uint8_t *hpplite_inflate(const uint8_t *in, size_t n, size_t *pOut) {
    size_t len;
    uLongf outLen;
    uint8_t *out;
    if (pOut) *pOut = 0;
    if (!in || n < 5) return NULL;
    len = ((size_t)in[0] << 24) | ((size_t)in[1] << 16) | ((size_t)in[2] << 8) | in[3];
    if (len == 0 || len > HPPLITE_INFLATE_MAX) return NULL;
    out = malloc(len + 1);
    if (!out) return NULL;
    outLen = (uLongf)len;
    if (uncompress(out, &outLen, in + 4, (uLong)(n - 4)) != Z_OK || outLen != len) { free(out); return NULL; }
    out[len] = 0;
    if (pOut) *pOut = len;
    return out;
}

int hpplite_is_plain_compressed(const uint8_t *data, size_t n) {
    return data && n > HPPLITE_PLAIN_MAGIC_LEN + 4 && memcmp(data, HPPLITE_PLAIN_MAGIC, HPPLITE_PLAIN_MAGIC_LEN) == 0;
}
