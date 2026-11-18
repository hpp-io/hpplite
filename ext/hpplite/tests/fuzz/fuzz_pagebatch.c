/*
** libFuzzer target (#343, somaro#136): the page batch decoder, which reads what the store or the chain serves.
** Any bytes: hpplite_pagebatch_is, _meta and _decode never read outside them, and every record the decoder returns
** points at pageSize bytes inside the input.
*/
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "pages.h"
#include "hpplite_sqlite.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    uint64_t ts = 0;
    int more = 0;
    volatile unsigned sink = 0;
    HpplitePageBatch *b;
    (void)hpplite_pagebatch_is(data, size);
    (void)hpplite_pagebatch_meta(data, size, &ts, &more);
    b = hpplite_pagebatch_decode(data, size);
    if (!b) return 0;
    for (unsigned i = 0; i < b->nPages; i++) {
        const unsigned char *body = b->aPage[i].body;
        /* a record's body lies within the input: touching its first and last byte is what ASan checks */
        if (body && b->pageSize) sink += body[0] + body[b->pageSize - 1];
    }
    if (b->zName && b->nName) sink += (unsigned char)b->zName[0] + (unsigned char)b->zName[b->nName - 1];
    (void)sink;
    hpplite_pagebatch_free(b);
    return 0;
}
