/*
** HPPLite Batch Implementation
**
** Batch creation, serialization, and parsing.
** Uses simple hand-rolled JSON for portability.
*/

#include "batch.h"
#include "hpplite_sqlite.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <ctype.h>

/*
** Convert bytes to hex string
*/
void hpplite_to_hex(const unsigned char *pData, int len, char *zOut) {
  static const char hexDigits[] = "0123456789abcdef";
  int i;
  for (i = 0; i < len; i++) {
    zOut[i*2] = hexDigits[(pData[i] >> 4) & 0xf];
    zOut[i*2 + 1] = hexDigits[pData[i] & 0xf];
  }
  zOut[len*2] = '\0';
}

/*
** Convert hex string to bytes
*/
int hpplite_from_hex(const char *zHex, unsigned char *pOut, int len) {
  int i;
  for (i = 0; i < len; i++) {
    int hi, lo;
    char c1 = zHex[i*2];
    char c2 = zHex[i*2 + 1];

    if (c1 >= '0' && c1 <= '9') hi = c1 - '0';
    else if (c1 >= 'a' && c1 <= 'f') hi = c1 - 'a' + 10;
    else if (c1 >= 'A' && c1 <= 'F') hi = c1 - 'A' + 10;
    else return -1;

    if (c2 >= '0' && c2 <= '9') lo = c2 - '0';
    else if (c2 >= 'a' && c2 <= 'f') lo = c2 - 'a' + 10;
    else if (c2 >= 'A' && c2 <= 'F') lo = c2 - 'A' + 10;
    else return -1;

    pOut[i] = (unsigned char)((hi << 4) | lo);
  }
  return 0;
}

/*
** Free a batch
*/
void hpplite_batch_free(HppliteBatch *pBatch) {
  if (pBatch) {
    int i;
    for (i = 0; i < pBatch->nTxns; i++) {
      sqlite3_free(pBatch->aTxns[i].zSql);
    }
    sqlite3_free(pBatch->aTxns);
    sqlite3_free(pBatch->zDbName);
    if (pBatch->bOwnsPosted) free((void *)pBatch->pPosted);
    sqlite3_free(pBatch->pPagesRaw);
    sqlite3_free(pBatch);
  }
}

/* === Checkpoint Implementation === */

/*
** Create a new checkpoint
*/
HppliteCheckpoint *hpplite_checkpoint_new(uint64_t fromHeight, uint64_t toHeight) {
  HppliteCheckpoint *pCheckpoint = sqlite3_malloc(sizeof(HppliteCheckpoint));
  if (pCheckpoint) {
    memset(pCheckpoint, 0, sizeof(HppliteCheckpoint));
    pCheckpoint->fromHeight = fromHeight;
    pCheckpoint->toHeight = toHeight;
    pCheckpoint->timestamp = (uint64_t)time(NULL);
  }
  return pCheckpoint;
}

/*
** Free a checkpoint
*/
void hpplite_checkpoint_free(HppliteCheckpoint *pCheckpoint) {
  if (pCheckpoint) {
    sqlite3_free(pCheckpoint);
  }
}
