/*
** HPPLite batch compression (#12): zlib's deflate, level 6. See compress.h
** for the posted form.
*/
#include "compress.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

uint8_t *hpplite_deflate_level(const uint8_t *in, size_t n, int level, size_t *pOut) {
    uLongf bound;
    uint8_t *out;
    if (pOut) *pOut = 0;
    if (!in || n == 0 || n > HPPLITE_INFLATE_MAX) return NULL;
    bound = compressBound((uLong)n);
    out = malloc(4 + bound);
    if (!out) return NULL;
    out[0] = (uint8_t)(n >> 24); out[1] = (uint8_t)(n >> 16); out[2] = (uint8_t)(n >> 8); out[3] = (uint8_t)n;
    if (compress2(out + 4, &bound, in, (uLong)n, level) != Z_OK) { free(out); return NULL; }
    if (pOut) *pOut = 4 + (size_t)bound;
    return out;
}

uint8_t *hpplite_deflate(const uint8_t *in, size_t n, size_t *pOut) { return hpplite_deflate_level(in, n, 6, pOut); }

#define DSTREAM_OUT (256 * 1024)
struct HppliteDeflateStream {
    z_stream z;
    int (*xOut)(void *, const uint8_t *, size_t);
    void *pArg;
    uint8_t out[DSTREAM_OUT];
};

HppliteDeflateStream *hpplite_deflate_stream_new(uint64_t totalLen, int level, int (*xOut)(void *, const uint8_t *, size_t), void *pArg) {
    HppliteDeflateStream *d;
    uint8_t len[4];
    if (totalLen == 0 || totalLen > 0xFFFFFFFFull || !xOut) return NULL;
    d = malloc(sizeof(*d));
    if (!d) return NULL;
    memset(&d->z, 0, sizeof(d->z));
    if (deflateInit(&d->z, level) != Z_OK) { free(d); return NULL; }
    d->xOut = xOut; d->pArg = pArg;
    len[0] = (uint8_t)(totalLen >> 24); len[1] = (uint8_t)(totalLen >> 16); len[2] = (uint8_t)(totalLen >> 8); len[3] = (uint8_t)totalLen;
    if (xOut(pArg, len, 4) != 0) { deflateEnd(&d->z); free(d); return NULL; }
    return d;
}

static int dstream_run(HppliteDeflateStream *d, int flush) {
    int zrc;
    do {
        d->z.next_out = d->out; d->z.avail_out = DSTREAM_OUT;
        zrc = deflate(&d->z, flush);
        if (zrc == Z_STREAM_ERROR) return -1;
        if (DSTREAM_OUT - d->z.avail_out > 0 && d->xOut(d->pArg, d->out, DSTREAM_OUT - d->z.avail_out) != 0) return -1;
    } while (d->z.avail_out == 0);
    return flush == Z_FINISH && zrc != Z_STREAM_END ? -1 : 0;
}

int hpplite_deflate_stream_write(HppliteDeflateStream *d, const uint8_t *in, size_t n) {
    while (n > 0) {
        uInt k = n > 0x40000000u ? 0x40000000u : (uInt)n;
        d->z.next_in = (Bytef *)in; d->z.avail_in = k;
        if (dstream_run(d, Z_NO_FLUSH) != 0) return -1;
        in += k; n -= k;
    }
    return 0;
}

int hpplite_deflate_stream_finish(HppliteDeflateStream *d) {
    int rc;
    d->z.next_in = NULL; d->z.avail_in = 0;
    rc = dstream_run(d, Z_FINISH);
    deflateEnd(&d->z);
    free(d);
    return rc;
}

void hpplite_deflate_stream_free(HppliteDeflateStream *d) { if (d) { deflateEnd(&d->z); free(d); } }

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
