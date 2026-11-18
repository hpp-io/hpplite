/*
** libFuzzer target (#343, somaro#136): the WAL reader, which reads frames another process wrote. The input is a
** database's -wal file; watched and polled, the reader never reads outside the file and every commit it hands the
** sink has nPages pages of pageSize bytes.
*/
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include "walread.h"

static unsigned gCommits;

static void sink(void *pArg, const HppliteWalCommit *c) {
    volatile unsigned *acc = pArg;
    gCommits++;
    for (unsigned i = 0; i < c->nPages; i++) {
        if (c->pageSize > 0) *acc += c->aImg[i][0] + c->aImg[i][c->pageSize - 1] + c->aPgno[i];
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static char path[64], wal[72];
    volatile unsigned acc = 0;
    FILE *f;
    if (!path[0]) {
        snprintf(path, sizeof path, "/tmp/hpplite_fuzz_wal_%d.db", (int)getpid());
        snprintf(wal, sizeof wal, "%s-wal", path);
    }
    if (!(f = fopen(wal, "wb"))) return 0;
    if (size && fwrite(data, 1, size, f) != size) { fclose(f); return 0; }
    fclose(f);
    hpplite_walread_watch(path, sink, (void *)&acc, 0);
    hpplite_walread_poll(path, 0);
    hpplite_walread_unwatch(path);
    unlink(wal);
    if (getenv("HPPLITE_FUZZ_SHOW")) printf("walread: %zu bytes, %u commits so far\n", size, gCommits);     /* do the seeds reach the sink? */
    return 0;
}
