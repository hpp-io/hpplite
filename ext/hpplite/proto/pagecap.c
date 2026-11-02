/*
** pagecap: a prototype of page-based capture (#146).
**
** A VFS shim between SQLite and the default VFS. It watches the WAL file a
** connection writes: every frame is a page image, the frame whose header
** carries a database size is the commit. At each commit the transaction's
** pages are handed to a capture that measures what shipping them would
** cost: raw, as an XOR diff against the previous image of the same page,
** and deflated (both). Loaded as an extension it registers itself as the
** default VFS, so every connection opened after it goes through it; files
** not in WAL mode pass through untouched.
**
**   PAGECAP_STATS=1   print the totals to stderr at exit
**   PAGECAP_NOWORK=1  only copy the pages (no diff, no deflate): the floor
**                     of the write-path cost, the rest being a flush thread's
**
** Prototype: single process, one database at a time per PcDb, no spool, no
** restart handling beyond resetting on a new WAL header.
*/
#include "sqlite3ext.h"
SQLITE_EXTENSION_INIT1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <zlib.h>

typedef struct PcDb PcDb;
struct PcDb {
  char *zPath;                 /* main database path */
  int pageSize;
  /* previous images, by page number */
  unsigned char **aPrev;
  unsigned nPrev;
  /* the transaction being written: frame index -> page number, and images */
  unsigned *aFramePgno; unsigned nFrames;
  unsigned char **aTxnImg; unsigned *aTxnPgno; unsigned nTxn, nTxnAlloc;
  /* totals */
  unsigned long long nCommits, nPages, rawBytes, diffBytes, rawZ, diffZ, workNs;
  /* batch accounting (PAGECAP_BATCH=N commits): the dirty set since the last batch, diffed against the image at that batch */
  unsigned char **aBatchPrev; unsigned nBatchPrev;
  unsigned char *aDirty; unsigned nDirtyAlloc;
  unsigned commitsInBatch;
  unsigned long long nBatches, batchPages, batchRaw, batchDiffZ;
  PcDb *pNext;
};

typedef struct PcFile PcFile;
struct PcFile {
  sqlite3_file base;
  sqlite3_file *pReal;
  int isWal;
  PcDb *pDb;
};

static sqlite3_vfs *g_orig;
static sqlite3_vfs g_vfs;
static PcDb *g_dbs;
static int g_nowork;
static unsigned g_batch;

static uint64_t now_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec; }
static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

static PcDb *dbFor(const char *zPath, int create) {
  PcDb *d;
  for (d = g_dbs; d; d = d->pNext) if (strcmp(d->zPath, zPath) == 0) return d;
  if (!create) return NULL;
  d = calloc(1, sizeof(*d));
  d->zPath = strdup(zPath);
  d->pNext = g_dbs; g_dbs = d;
  return d;
}

static size_t deflated(const unsigned char *p, size_t n, unsigned char *buf, size_t cap) {
  z_stream z; memset(&z, 0, sizeof z);
  if (deflateInit(&z, 6) != Z_OK) return n;
  z.next_in = (Bytef *)p; z.avail_in = (uInt)n; z.next_out = buf; z.avail_out = (uInt)cap;
  deflate(&z, Z_FINISH);
  { size_t out = cap - z.avail_out; deflateEnd(&z); return out; }
}

/* a batch closes: the dirty pages since the last batch, each as the diff of
** its current image against the image the previous batch left, deflated */
static void batchClose(PcDb *d) {
  unsigned pg, n = 0;
  size_t ps = (size_t)d->pageSize, len;
  unsigned char *diff, *zbuf;
  for (pg = 0; pg < d->nDirtyAlloc; pg++) if (d->aDirty[pg]) n++;
  if (!n) { d->commitsInBatch = 0; return; }
  len = (size_t)n * ps; diff = malloc(len); zbuf = malloc(len + len / 8 + 64);
  { unsigned i = 0; for (pg = 0; pg < d->nDirtyAlloc; pg++) if (d->aDirty[pg]) {
      unsigned char *cur = d->aPrev[pg], *old = pg < d->nBatchPrev ? d->aBatchPrev[pg] : NULL;
      size_t k;
      if (old) for (k = 0; k < ps; k++) diff[i * ps + k] = cur[k] ^ old[k]; else memcpy(diff + i * ps, cur, ps);
      if (pg >= d->nBatchPrev) { unsigned m = d->nBatchPrev ? d->nBatchPrev : 1024; while (m <= pg) m *= 2; d->aBatchPrev = realloc(d->aBatchPrev, m * sizeof(*d->aBatchPrev)); memset(d->aBatchPrev + d->nBatchPrev, 0, (m - d->nBatchPrev) * sizeof(*d->aBatchPrev)); d->nBatchPrev = m; }
      if (!d->aBatchPrev[pg]) d->aBatchPrev[pg] = malloc(ps);
      memcpy(d->aBatchPrev[pg], cur, ps);
      d->aDirty[pg] = 0; i++;
  } }
  d->nBatches++; d->batchPages += n; d->batchRaw += len;
  d->batchDiffZ += deflated(diff, len, zbuf, len + len / 8 + 64);
  free(diff); free(zbuf);
  d->commitsInBatch = 0;
}

/* the commit: measure the transaction's pages, remember them as previous */
static void txnCommit(PcDb *d) {
  unsigned i;
  uint64_t t0 = now_ns();
  size_t ps = (size_t)d->pageSize;
  unsigned char *raw = NULL, *diff = NULL, *zbuf = NULL;
  size_t rawLen = (size_t)d->nTxn * ps;
  if (!g_nowork) {
    raw = malloc(rawLen); diff = malloc(rawLen); zbuf = malloc(rawLen + rawLen / 8 + 64);
  }
  for (i = 0; i < d->nTxn; i++) {
    unsigned pg = d->aTxnPgno[i];
    unsigned char *img = d->aTxnImg[i];
    if (pg >= d->nPrev) {
      unsigned n = d->nPrev ? d->nPrev : 1024;
      while (n <= pg) n *= 2;
      d->aPrev = realloc(d->aPrev, n * sizeof(*d->aPrev));
      memset(d->aPrev + d->nPrev, 0, (n - d->nPrev) * sizeof(*d->aPrev));
      d->nPrev = n;
    }
    if (!g_nowork) {
      size_t k;
      unsigned char *prev = d->aPrev[pg];
      memcpy(raw + i * ps, img, ps);
      if (prev) for (k = 0; k < ps; k++) diff[i * ps + k] = img[k] ^ prev[k];
      else memcpy(diff + i * ps, img, ps);
    }
    if (!d->aPrev[pg]) d->aPrev[pg] = malloc(ps);
    memcpy(d->aPrev[pg], img, ps);
  }
  d->nCommits++;
  d->nPages += d->nTxn;
  d->rawBytes += rawLen;
  if (g_batch) {
    for (i = 0; i < d->nTxn; i++) {
      unsigned pg = d->aTxnPgno[i];
      if (pg >= d->nDirtyAlloc) { unsigned n = d->nDirtyAlloc ? d->nDirtyAlloc : 1024; while (n <= pg) n *= 2; d->aDirty = realloc(d->aDirty, n); memset(d->aDirty + d->nDirtyAlloc, 0, n - d->nDirtyAlloc); d->nDirtyAlloc = n; }
      d->aDirty[pg] = 1;
    }
    if (++d->commitsInBatch >= g_batch) batchClose(d);
  }
  if (!g_nowork) {
    d->rawZ += deflated(raw, rawLen, zbuf, rawLen + rawLen / 8 + 64);
    d->diffZ += deflated(diff, rawLen, zbuf, rawLen + rawLen / 8 + 64);
    { size_t k, nz = 0; for (k = 0; k < rawLen; k++) if (diff[k]) nz++; d->diffBytes += nz; }
  }
  free(raw); free(diff); free(zbuf);
  for (i = 0; i < d->nTxn; i++) free(d->aTxnImg[i]);
  d->nTxn = 0;
  d->workNs += now_ns() - t0;
}

static void txnAddPage(PcDb *d, unsigned pg, const unsigned char *img) {
  unsigned i;
  for (i = 0; i < d->nTxn; i++) if (d->aTxnPgno[i] == pg) { memcpy(d->aTxnImg[i], img, (size_t)d->pageSize); return; }
  if (d->nTxn == d->nTxnAlloc) {
    d->nTxnAlloc = d->nTxnAlloc ? d->nTxnAlloc * 2 : 64;
    d->aTxnImg = realloc(d->aTxnImg, d->nTxnAlloc * sizeof(*d->aTxnImg));
    d->aTxnPgno = realloc(d->aTxnPgno, d->nTxnAlloc * sizeof(*d->aTxnPgno));
  }
  d->aTxnImg[d->nTxn] = malloc((size_t)d->pageSize);
  memcpy(d->aTxnImg[d->nTxn], img, (size_t)d->pageSize);
  d->aTxnPgno[d->nTxn] = pg;
  d->nTxn++;
}

/* a write to the WAL file: the header, a frame header, or a frame's page */
static void walWrite(PcDb *d, const unsigned char *p, int amt, sqlite3_int64 off) {
  if (off == 0 && amt >= 32) {
    d->pageSize = (int)be32(p + 8);
    d->nFrames = 0; d->nTxn = 0;                     /* a new WAL (or a restart): frames start over */
    if (amt == 32) return;
    p += 32; amt -= 32; off = 32;
  }
  if (d->pageSize <= 0) return;
  while (amt > 0) {
    sqlite3_int64 frameSz = 24 + d->pageSize;
    sqlite3_int64 rel = off - 32;
    unsigned idx = (unsigned)(rel / frameSz);
    int within = (int)(rel % frameSz);
    if (within == 0 && amt >= 24) {
      unsigned pg = be32(p), nTrunc = be32(p + 4);
      if (idx >= d->nFrames) {
        unsigned n = idx + 1;
        d->aFramePgno = realloc(d->aFramePgno, n * sizeof(unsigned));
        d->nFrames = n;
      }
      d->aFramePgno[idx] = pg;
      if (amt >= 24 + d->pageSize) {                 /* header and page in one write */
        txnAddPage(d, pg, p + 24);
        if (nTrunc) txnCommit(d);
        p += 24 + d->pageSize; amt -= 24 + d->pageSize; off += 24 + d->pageSize;
        continue;
      }
      /* header alone: the page follows in the next write; commit decided then */
      d->aFramePgno[idx] = pg | (nTrunc ? 0x80000000u : 0);
      p += 24; amt -= 24; off += 24;
      continue;
    }
    if (within == 24 && amt >= d->pageSize && idx < d->nFrames) {
      unsigned pg = d->aFramePgno[idx] & 0x7fffffffu, commit = d->aFramePgno[idx] & 0x80000000u;
      txnAddPage(d, pg, p);
      if (commit) txnCommit(d);
      p += d->pageSize; amt -= d->pageSize; off += d->pageSize;
      continue;
    }
    break;                                           /* a partial write we do not model */
  }
}

/* ---- io methods: forwarders, the WAL write watched ---- */
#define REAL(f) (((PcFile *)(f))->pReal)
static int pcClose(sqlite3_file *f) { int rc = REAL(f)->pMethods->xClose(REAL(f)); return rc; }
static int pcRead(sqlite3_file *f, void *b, int n, sqlite3_int64 o) { return REAL(f)->pMethods->xRead(REAL(f), b, n, o); }
static int pcWrite(sqlite3_file *f, const void *b, int n, sqlite3_int64 o) {
  PcFile *p = (PcFile *)f;
  int rc = REAL(f)->pMethods->xWrite(REAL(f), b, n, o);
  if (rc == SQLITE_OK && p->isWal && p->pDb) walWrite(p->pDb, (const unsigned char *)b, n, o);
  return rc;
}
static int pcTruncate(sqlite3_file *f, sqlite3_int64 n) { return REAL(f)->pMethods->xTruncate(REAL(f), n); }
static int pcSync(sqlite3_file *f, int flags) { return REAL(f)->pMethods->xSync(REAL(f), flags); }
static int pcFileSize(sqlite3_file *f, sqlite3_int64 *n) { return REAL(f)->pMethods->xFileSize(REAL(f), n); }
static int pcLock(sqlite3_file *f, int l) { return REAL(f)->pMethods->xLock(REAL(f), l); }
static int pcUnlock(sqlite3_file *f, int l) { return REAL(f)->pMethods->xUnlock(REAL(f), l); }
static int pcCheckReservedLock(sqlite3_file *f, int *r) { return REAL(f)->pMethods->xCheckReservedLock(REAL(f), r); }
static int pcFileControl(sqlite3_file *f, int op, void *a) { return REAL(f)->pMethods->xFileControl(REAL(f), op, a); }
static int pcSectorSize(sqlite3_file *f) { return REAL(f)->pMethods->xSectorSize(REAL(f)); }
static int pcDeviceCharacteristics(sqlite3_file *f) { return REAL(f)->pMethods->xDeviceCharacteristics(REAL(f)); }
static int pcShmMap(sqlite3_file *f, int i, int sz, int ext, void volatile **pp) { return REAL(f)->pMethods->xShmMap(REAL(f), i, sz, ext, pp); }
static int pcShmLock(sqlite3_file *f, int o, int n, int fl) { return REAL(f)->pMethods->xShmLock(REAL(f), o, n, fl); }
static void pcShmBarrier(sqlite3_file *f) { REAL(f)->pMethods->xShmBarrier(REAL(f)); }
static int pcShmUnmap(sqlite3_file *f, int del) { return REAL(f)->pMethods->xShmUnmap(REAL(f), del); }
static int pcFetch(sqlite3_file *f, sqlite3_int64 o, int n, void **pp) { return REAL(f)->pMethods->xFetch ? REAL(f)->pMethods->xFetch(REAL(f), o, n, pp) : SQLITE_OK; }
static int pcUnfetch(sqlite3_file *f, sqlite3_int64 o, void *p) { return REAL(f)->pMethods->xUnfetch ? REAL(f)->pMethods->xUnfetch(REAL(f), o, p) : SQLITE_OK; }

static const sqlite3_io_methods pc_io = {
  3, pcClose, pcRead, pcWrite, pcTruncate, pcSync, pcFileSize, pcLock, pcUnlock, pcCheckReservedLock,
  pcFileControl, pcSectorSize, pcDeviceCharacteristics, pcShmMap, pcShmLock, pcShmBarrier, pcShmUnmap, pcFetch, pcUnfetch
};

static int pcOpen(sqlite3_vfs *vfs, sqlite3_filename zName, sqlite3_file *f, int flags, int *pOut) {
  PcFile *p = (PcFile *)f;
  int rc;
  (void)vfs;
  p->pReal = (sqlite3_file *)&p[1];
  p->isWal = 0; p->pDb = NULL;
  rc = g_orig->xOpen(g_orig, zName, p->pReal, flags, pOut);
  if (rc != SQLITE_OK) return rc;
  if (zName) {
    if (flags & SQLITE_OPEN_MAIN_DB) dbFor(zName, 1);
    else if (flags & SQLITE_OPEN_WAL) {
      size_t n = strlen(zName);
      if (n > 4 && strcmp(zName + n - 4, "-wal") == 0) {
        char *zDb = sqlite3_mprintf("%.*s", (int)(n - 4), zName);
        p->pDb = dbFor(zDb, 1);
        sqlite3_free(zDb);
        p->isWal = 1;
      }
    }
  }
  /* a parent file with fewer methods: our table forwards only what exists */
  f->pMethods = &pc_io;
  return SQLITE_OK;
}
static int pcDelete(sqlite3_vfs *v, const char *z, int s) { (void)v; return g_orig->xDelete(g_orig, z, s); }
static int pcAccess(sqlite3_vfs *v, const char *z, int fl, int *r) { (void)v; return g_orig->xAccess(g_orig, z, fl, r); }
static int pcFullPathname(sqlite3_vfs *v, const char *z, int n, char *o) { (void)v; return g_orig->xFullPathname(g_orig, z, n, o); }
static void *pcDlOpen(sqlite3_vfs *v, const char *z) { (void)v; return g_orig->xDlOpen(g_orig, z); }
static void pcDlError(sqlite3_vfs *v, int n, char *z) { (void)v; g_orig->xDlError(g_orig, n, z); }
static void (*pcDlSym(sqlite3_vfs *v, void *h, const char *z))(void) { (void)v; return g_orig->xDlSym(g_orig, h, z); }
static void pcDlClose(sqlite3_vfs *v, void *h) { (void)v; g_orig->xDlClose(g_orig, h); }
static int pcRandomness(sqlite3_vfs *v, int n, char *z) { (void)v; return g_orig->xRandomness(g_orig, n, z); }
static int pcSleep(sqlite3_vfs *v, int us) { (void)v; return g_orig->xSleep(g_orig, us); }
static int pcCurrentTime(sqlite3_vfs *v, double *t) { (void)v; return g_orig->xCurrentTime(g_orig, t); }
static int pcGetLastError(sqlite3_vfs *v, int n, char *z) { (void)v; return g_orig->xGetLastError ? g_orig->xGetLastError(g_orig, n, z) : 0; }
static int pcCurrentTimeInt64(sqlite3_vfs *v, sqlite3_int64 *t) { (void)v; return g_orig->xCurrentTimeInt64 ? g_orig->xCurrentTimeInt64(g_orig, t) : g_orig->xCurrentTime(g_orig, (double *)t); }

static void printStats(void) {
  PcDb *d;
  for (d = g_dbs; d; d = d->pNext) {
    if (!d->nCommits) continue;
    if (g_batch && d->commitsInBatch) batchClose(d);
    if (g_batch) fprintf(stderr, "pagecap batches of %u commits: %llu batches, %.1f dirty pages per batch (%.1f per commit), per batch raw %.0f B, diff deflated %.0f B (%.0f B per commit)\n",
            g_batch, d->nBatches, (double)d->batchPages / (double)(d->nBatches ? d->nBatches : 1), (double)d->batchPages / (double)d->nCommits,
            (double)d->batchRaw / (double)(d->nBatches ? d->nBatches : 1), (double)d->batchDiffZ / (double)(d->nBatches ? d->nBatches : 1), (double)d->batchDiffZ / (double)d->nCommits);
    fprintf(stderr, "pagecap %s: commits=%llu pages=%llu (%.1f per commit, page %d B)  raw=%.1f MB  diff nonzero=%.1f MB  raw deflated=%.1f MB  diff deflated=%.1f MB  per commit: raw %.0f B, diff deflated %.0f B  work %.1f ms (%.1f us per commit)%s\n",
            d->zPath, d->nCommits, d->nPages, (double)d->nPages / (double)d->nCommits, d->pageSize,
            d->rawBytes / 1048576.0, d->diffBytes / 1048576.0, d->rawZ / 1048576.0, d->diffZ / 1048576.0,
            (double)d->rawBytes / (double)d->nCommits, (double)d->diffZ / (double)d->nCommits,
            d->workNs / 1e6, d->workNs / 1e3 / (double)d->nCommits, g_nowork ? " [nowork: copies only]" : "");
  }
}

static void statsFunc(sqlite3_context *ctx, int argc, sqlite3_value **argv) {
  PcDb *d; char *z = sqlite3_mprintf("");
  (void)argc; (void)argv;
  for (d = g_dbs; d; d = d->pNext) {
    char *n = sqlite3_mprintf("%z%s: commits=%llu pages=%llu raw=%llu diffz=%llu rawz=%llu\n", z, d->zPath, d->nCommits, d->nPages, d->rawBytes, d->diffZ, d->rawZ);
    z = n;
  }
  sqlite3_result_text(ctx, z, -1, sqlite3_free);
}

#ifdef _WIN32
__declspec(dllexport)
#endif
int sqlite3_pagecap_init(sqlite3 *db, char **pzErr, const sqlite3_api_routines *pApi) {
  SQLITE_EXTENSION_INIT2(pApi);
  (void)pzErr;
  if (!g_orig) {
    g_orig = sqlite3_vfs_find(NULL);
    g_vfs = *g_orig;
    g_vfs.zName = "pagecap";
    g_vfs.pNext = NULL;
    g_vfs.szOsFile = (int)sizeof(PcFile) + g_orig->szOsFile;
    g_vfs.xOpen = pcOpen; g_vfs.xDelete = pcDelete; g_vfs.xAccess = pcAccess; g_vfs.xFullPathname = pcFullPathname;
    g_vfs.xDlOpen = pcDlOpen; g_vfs.xDlError = pcDlError; g_vfs.xDlSym = pcDlSym; g_vfs.xDlClose = pcDlClose;
    g_vfs.xRandomness = pcRandomness; g_vfs.xSleep = pcSleep; g_vfs.xCurrentTime = pcCurrentTime;
    g_vfs.xGetLastError = pcGetLastError; g_vfs.xCurrentTimeInt64 = pcCurrentTimeInt64;
    g_vfs.xSetSystemCall = NULL; g_vfs.xGetSystemCall = NULL; g_vfs.xNextSystemCall = NULL;
    sqlite3_vfs_register(&g_vfs, 1);
    g_nowork = getenv("PAGECAP_NOWORK") != NULL;
    g_batch = getenv("PAGECAP_BATCH") ? (unsigned)atoi(getenv("PAGECAP_BATCH")) : 0;
    if (getenv("PAGECAP_STATS")) atexit(printStats);
  }
  sqlite3_create_function(db, "pagecap_stats", 0, SQLITE_UTF8, NULL, statsFunc, NULL, NULL);
  return SQLITE_OK;
}
