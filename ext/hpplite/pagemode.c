/* Page mode (#146). See pagemode.h. */
#include "hpplite_sqlite.h"
#include "pagemode.h"
#include "pages.h"
#include "walread.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

typedef struct QCommit QCommit;
struct QCommit {
  unsigned n;                 /* pages */
  unsigned *aPgno;
  unsigned char *aImg;        /* n images, back to back */
  unsigned nTrunc;            /* the file's size after, in pages */
  unsigned salt1, salt2;
  QCommit *pNext;
};

struct HpplitePageMode {
  char *zPath;
  int pageSize;
  pthread_mutex_t mu;         /* the queue and the counts: the sink's thread and the flush thread */
  QCommit *pHead, *pTail;
  unsigned nQueued;
  size_t queuedBytes;
  unsigned maxPgQueued;       /* the highest page number among queued commits: the build's arrays, without a scan of the queue */
  uint64_t nCommits;          /* captured since start */
  uint64_t nCaptured;         /* ...of which the sink's: the file as opened (a snapshot) is not one */
  uint64_t nFinalized;        /* in batches finalized */
  /* the flush thread's: the state after the last finalized batch */
  HpplitePageRoot *pRoot;
  unsigned char **aPrev; unsigned nPrev;      /* each page's image then (NULL: beyond the file) */
  unsigned sizeThen;                          /* the file's size in pages then */
  /* the batch in flight */
  QCommit *pBuilding; unsigned nBuilding;
  unsigned *aDirty; unsigned nDirty;          /* its dirty pages, each with its last image in the building commits */
  unsigned char **aLast;
  unsigned sizeAfter;
  int bWatch;
  char *zName;                /* on the next batch built: the database's name (#160) */
  unsigned takeExactly;       /* a build of exactly this many commits (0: by the budget) */
};

static void qfree(QCommit *q) { while (q) { QCommit *n = q->pNext; sqlite3_free(q->aPgno); sqlite3_free(q->aImg); sqlite3_free(q); q = n; } }

/* the sink: the commit's pages, queued */
static void sink(void *pArg, const HppliteWalCommit *c) {
  HpplitePageMode *pm = pArg;
  QCommit *q = sqlite3_malloc64(sizeof(*q));
  unsigned i;
  if (!q) return;
  memset(q, 0, sizeof(*q));
  q->n = c->nPages; q->nTrunc = c->nTruncate; q->salt1 = c->salt1; q->salt2 = c->salt2;
  q->aPgno = sqlite3_malloc64((sqlite3_uint64)c->nPages * sizeof(unsigned));
  q->aImg = sqlite3_malloc64((sqlite3_uint64)c->nPages * (sqlite3_uint64)c->pageSize);
  if (!q->aPgno || !q->aImg) { qfree(q); return; }
  for (i = 0; i < c->nPages; i++) { q->aPgno[i] = c->aPgno[i]; memcpy(q->aImg + (size_t)i * (size_t)c->pageSize, c->aImg[i], (size_t)c->pageSize); }
  pthread_mutex_lock(&pm->mu);
  for (i = 0; i < c->nPages; i++) if (c->aPgno[i] > pm->maxPgQueued) pm->maxPgQueued = c->aPgno[i];
  if (pm->pageSize == 0) pm->pageSize = c->pageSize;
  if (pm->pTail) pm->pTail->pNext = q; else pm->pHead = q;
  pm->pTail = q; pm->nQueued++; pm->queuedBytes += (size_t)c->nPages * (size_t)c->pageSize; pm->nCommits++; pm->nCaptured++;
  pthread_mutex_unlock(&pm->mu);
}

static int ensurePrev(HpplitePageMode *pm, unsigned n) {
  if (n <= pm->nPrev) return SQLITE_OK;
  { unsigned cap = pm->nPrev ? pm->nPrev : 256; while (cap < n) cap *= 2;
    unsigned char **a = sqlite3_realloc64(pm->aPrev, (sqlite3_uint64)cap * sizeof(*a));
    if (!a) return SQLITE_NOMEM;
    memset(a + pm->nPrev, 0, (size_t)(cap - pm->nPrev) * sizeof(*a));
    pm->aPrev = a; pm->nPrev = cap; }
  return SQLITE_OK;
}

HpplitePageMode *hpplite_pagemode_start(const char *zPath, int bWatch, unsigned startFrame) {
  HpplitePageMode *pm = sqlite3_malloc64(sizeof(*pm));
  FILE *f;
  unsigned char hdr[100];
  unsigned i, n;
  if (!pm) return NULL;
  memset(pm, 0, sizeof(*pm));
  pthread_mutex_init(&pm->mu, NULL);
  pm->zPath = sqlite3_mprintf("%s", zPath);
  /* the file as it is: every page's image, the root */
  f = fopen(zPath, "rb");
  if (f && fread(hdr, 1, 100, f) == 100) {
    pm->pageSize = (hdr[16] << 8) | hdr[17];
    if (pm->pageSize == 1) pm->pageSize = 65536;
    n = ((unsigned)hdr[28] << 24) | ((unsigned)hdr[29] << 16) | ((unsigned)hdr[30] << 8) | hdr[31];
    pm->pRoot = hpplite_pageroot_new(pm->pageSize);
    if (pm->pRoot && ensurePrev(pm, n) == SQLITE_OK) {
      fseek(f, 0, SEEK_SET);
      for (i = 1; i <= n; i++) {
        unsigned char *img = sqlite3_malloc64((sqlite3_uint64)pm->pageSize);
        if (!img || fread(img, 1, (size_t)pm->pageSize, f) != (size_t)pm->pageSize) { sqlite3_free(img); break; }
        pm->aPrev[i - 1] = img;
        hpplite_pageroot_set(pm->pRoot, i, img);
      }
      pm->sizeThen = i - 1;
    }
  } else {
    pm->pRoot = hpplite_pageroot_new(0);          /* an empty file: the first commit says the page size */
  }
  if (f) fclose(f);
  if (!pm->pRoot) { hpplite_pagemode_stop(pm); return NULL; }
  pm->bWatch = bWatch;
  if (bWatch) hpplite_walread_watch(zPath, sink, pm, startFrame);
  return pm;
}

void hpplite_pagemode_pause(HpplitePageMode *pm) {
  if (pm && pm->bWatch && pm->zPath) hpplite_walread_unwatch(pm->zPath);
}
void hpplite_pagemode_resume(HpplitePageMode *pm) {
  if (pm && pm->bWatch && pm->zPath) hpplite_walread_watch(pm->zPath, sink, pm, 0);
}

int hpplite_pagemode_snapshot(HpplitePageMode *pm) {
  QCommit *q;
  unsigned i, n;
  if (!pm || !pm->pRoot || pm->pageSize <= 0) return SQLITE_MISUSE;
  n = pm->sizeThen;
  if (n == 0) return SQLITE_OK;
  q = sqlite3_malloc64(sizeof(*q));
  if (!q) return SQLITE_NOMEM;
  memset(q, 0, sizeof(*q));
  q->n = n; q->nTrunc = n;
  q->aPgno = sqlite3_malloc64((sqlite3_uint64)n * sizeof(unsigned));
  q->aImg = sqlite3_malloc64((sqlite3_uint64)n * (sqlite3_uint64)pm->pageSize);
  if (!q->aPgno || !q->aImg) { qfree(q); return SQLITE_NOMEM; }
  for (i = 0; i < n; i++) {
    q->aPgno[i] = i + 1;
    memcpy(q->aImg + (size_t)i * (size_t)pm->pageSize, pm->aPrev[i], (size_t)pm->pageSize);
    sqlite3_free(pm->aPrev[i]); pm->aPrev[i] = NULL;          /* never shipped: the batch carries them whole */
  }
  pthread_mutex_lock(&pm->mu);
  q->pNext = pm->pHead; pm->pHead = q; if (!pm->pTail) pm->pTail = q;
  pm->nQueued++; pm->queuedBytes += (size_t)n * (size_t)pm->pageSize; pm->nCommits++;
  if (n > pm->maxPgQueued) pm->maxPgQueued = n;
  pthread_mutex_unlock(&pm->mu);
  return SQLITE_OK;
}

int hpplite_pagemode_applied(HpplitePageMode *pm, unsigned pgno, const unsigned char *page) {
  if (!pm || !pm->pRoot) return SQLITE_MISUSE;
  if (pm->pageSize == 0 && pgno == 1) { pm->pageSize = (page[16] << 8) | page[17]; if (pm->pageSize == 1) pm->pageSize = 65536; hpplite_pageroot_free(pm->pRoot); pm->pRoot = hpplite_pageroot_new(pm->pageSize); }
  return hpplite_pageroot_set(pm->pRoot, pgno, page);
}
int hpplite_pagemode_applied_size(HpplitePageMode *pm, unsigned nPages) {
  if (!pm || !pm->pRoot) return SQLITE_MISUSE;
  pm->sizeThen = nPages;
  return hpplite_pageroot_truncate(pm->pRoot, nPages);
}

void hpplite_pagemode_stop(HpplitePageMode *pm) {
  unsigned i;
  if (!pm) return;
  if (pm->zPath && pm->bWatch) hpplite_walread_unwatch(pm->zPath);
  pthread_mutex_lock(&pm->mu);
  qfree(pm->pHead); pm->pHead = pm->pTail = NULL;
  pthread_mutex_unlock(&pm->mu);
  qfree(pm->pBuilding);
  for (i = 0; i < pm->nPrev; i++) sqlite3_free(pm->aPrev[i]);
  sqlite3_free(pm->aPrev); sqlite3_free(pm->aDirty); sqlite3_free(pm->aLast);
  hpplite_pageroot_free(pm->pRoot);
  pthread_mutex_destroy(&pm->mu);
  sqlite3_free(pm->zPath);
  sqlite3_free(pm->zName);
  sqlite3_free(pm);
}

unsigned hpplite_pagemode_pending(HpplitePageMode *pm) {
  unsigned n;
  if (!pm) return 0;
  pthread_mutex_lock(&pm->mu); n = pm->nQueued + pm->nBuilding; pthread_mutex_unlock(&pm->mu);
  return n;
}
uint64_t hpplite_pagemode_captured(HpplitePageMode *pm) {
  uint64_t n;
  if (!pm) return 0;
  pthread_mutex_lock(&pm->mu); n = pm->nCaptured; pthread_mutex_unlock(&pm->mu);
  return n;
}
void hpplite_pagemode_set_name(HpplitePageMode *pm, const char *zName) {
  if (!pm) return;
  sqlite3_free(pm->zName);
  pm->zName = zName && zName[0] ? sqlite3_mprintf("%.255s", zName) : NULL;
}
void hpplite_pagemode_discard(HpplitePageMode *pm) {
  if (!pm || pm->pBuilding) return;
  pthread_mutex_lock(&pm->mu);
  qfree(pm->pHead); pm->pHead = pm->pTail = NULL; pm->nQueued = 0; pm->queuedBytes = 0; pm->maxPgQueued = 0;
  pthread_mutex_unlock(&pm->mu);
}
size_t hpplite_pagemode_pending_bytes(HpplitePageMode *pm) {
  size_t n;
  if (!pm) return 0;
  pthread_mutex_lock(&pm->mu); n = pm->queuedBytes; pthread_mutex_unlock(&pm->mu);
  return n;
}
int hpplite_pagemode_page_size(HpplitePageMode *pm) { return pm ? pm->pageSize : 0; }
void hpplite_pagemode_root(HpplitePageMode *pm, unsigned char out[32]) { hpplite_pageroot_get(pm ? pm->pRoot : NULL, out); }

static uint64_t now_ms(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000; }

int hpplite_pagemode_build(HpplitePageMode *pm, size_t maxBytes, unsigned char **ppBytes, size_t *pLen, unsigned char root[32], int *pMore) {
  QCommit *q, *last = NULL;
  unsigned nTake = 0, maxPg = 0, i, pg, ps;
  size_t recSize, budget;
  unsigned char *seen;
  HpplitePageBatch b;
  HpplitePageRec *recs;
  unsigned char *bodies;
  unsigned nRec = 0;
  *ppBytes = NULL; *pLen = 0; *pMore = 0;
  if (!pm || pm->pBuilding) return SQLITE_MISUSE;
  pthread_mutex_lock(&pm->mu);
  if (!pm->pHead) { pthread_mutex_unlock(&pm->mu); return SQLITE_DONE; }
  ps = (unsigned)pm->pageSize;
  recSize = 6 + (size_t)ps;
  budget = maxBytes > 96 + recSize ? (maxBytes - 96) / recSize : 1;      /* records that fit beside the header and trailer */
  /* commits from the head while their distinct pages fit; at least one */
  maxPg = pm->maxPgQueued;
  seen = sqlite3_malloc64(maxPg + 1);
  if (!seen) { pthread_mutex_unlock(&pm->mu); return SQLITE_NOMEM; }
  memset(seen, 0, maxPg + 1);
  { unsigned distinct = 0;
    for (q = pm->pHead; q; q = q->pNext) {
      unsigned added = 0;
      for (i = 0; i < q->n; i++) if (!seen[q->aPgno[i]]) added++;
      if (pm->takeExactly ? nTake >= pm->takeExactly : (nTake > 0 && distinct + added > budget)) break;
      for (i = 0; i < q->n; i++) seen[q->aPgno[i]] = 1;
      distinct += added; nTake++; last = q;
    } }
  pm->pBuilding = pm->pHead;
  pm->pHead = last->pNext; last->pNext = NULL;
  if (!pm->pHead) pm->pTail = NULL;
  pm->nBuilding = nTake; pm->nQueued -= nTake;
  for (q = pm->pBuilding; q; q = q->pNext) pm->queuedBytes -= (size_t)q->n * (size_t)ps;
  *pMore = pm->pHead != NULL;
  if (!pm->pHead) pm->maxPgQueued = 0;
  pthread_mutex_unlock(&pm->mu);

  /* the dirty set and each page's last image; the size after */
  memset(seen, 0, maxPg + 1);
  pm->aDirty = sqlite3_realloc64(pm->aDirty, (sqlite3_uint64)(maxPg + 1) * sizeof(unsigned));
  pm->aLast = sqlite3_realloc64(pm->aLast, (sqlite3_uint64)(maxPg + 1) * sizeof(unsigned char *));
  pm->nDirty = 0;
  for (q = pm->pBuilding; q; q = q->pNext) {
    for (i = 0; i < q->n; i++) {
      pg = q->aPgno[i];
      if (!seen[pg]) { seen[pg] = 1; pm->aDirty[pm->nDirty++] = pg; }
      pm->aLast[pg] = q->aImg + (size_t)i * (size_t)ps;
    }
    pm->sizeAfter = q->nTrunc;
  }
  sqlite3_free(seen);
  /* the root after: dirty pages within the file set, the file cut to its size */
  if (ensurePrev(pm, pm->sizeAfter > maxPg ? pm->sizeAfter : maxPg) != SQLITE_OK) { hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  recs = sqlite3_malloc64((sqlite3_uint64)(pm->nDirty ? pm->nDirty : 1) * sizeof(HpplitePageRec));
  bodies = sqlite3_malloc64((sqlite3_uint64)(pm->nDirty ? pm->nDirty : 1) * ps);
  if (!recs || !bodies) { sqlite3_free(recs); sqlite3_free(bodies); hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  /* records in page order */
  seen = sqlite3_malloc64(maxPg + 1);
  if (!seen) { sqlite3_free(recs); sqlite3_free(bodies); hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  memset(seen, 0, maxPg + 1);
  { unsigned k; for (k = 0; k < pm->nDirty; k++) seen[pm->aDirty[k]] = 1; }
  for (pg = 1; pg <= maxPg; pg++) {
    if (!seen[pg] || pg > pm->sizeAfter) continue;
    { unsigned char *body = bodies + (size_t)nRec * ps, *cur = pm->aLast[pg], *old = pm->aPrev[pg - 1];
      if (old) { unsigned j; for (j = 0; j < ps; j++) body[j] = cur[j] ^ old[j]; recs[nRec].flags = HPPLITE_PAGE_DIFF; }
      else { memcpy(body, cur, ps); recs[nRec].flags = HPPLITE_PAGE_FULL; }
      recs[nRec].pgno = pg; recs[nRec].body = body; nRec++;
      hpplite_pageroot_set(pm->pRoot, pg, cur); }
  }
  sqlite3_free(seen);
  hpplite_pageroot_truncate(pm->pRoot, pm->sizeAfter);
  memset(&b, 0, sizeof b);
  b.pageSize = ps; b.commitSize = pm->sizeAfter;
  b.firstCommit = pm->nFinalized + 1; b.lastCommit = pm->nFinalized + nTake;
  b.salt1 = last->salt1; b.salt2 = last->salt2; b.timestampMs = now_ms();
  b.nPages = nRec; b.aPage = recs;
  b.zName = pm->zName; b.nName = pm->zName ? (unsigned)strlen(pm->zName) : 0;
  hpplite_pageroot_get(pm->pRoot, b.root); b.crc = hpplite_pageroot_crc(pm->pRoot);
  memcpy(root, b.root, 32);
  *ppBytes = hpplite_pagebatch_encode(&b, pLen);
  sqlite3_free(recs); sqlite3_free(bodies);
  if (!*ppBytes) { hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  return SQLITE_OK;
}

int hpplite_pagemode_build_n(HpplitePageMode *pm, unsigned nCommits, unsigned char **ppBytes, size_t *pLen, unsigned char root[32], int *pMore) {
  int rc;
  if (!pm || nCommits == 0) return SQLITE_MISUSE;
  pm->takeExactly = nCommits;
  rc = hpplite_pagemode_build(pm, (size_t)-1 / 2, ppBytes, pLen, root, pMore);
  pm->takeExactly = 0;
  return rc;
}

void hpplite_pagemode_finalized(HpplitePageMode *pm) {
  unsigned k, pg;
  if (!pm || !pm->pBuilding) return;
  for (k = 0; k < pm->nDirty; k++) {
    pg = pm->aDirty[k];
    if (pg > pm->sizeAfter) continue;
    if (!pm->aPrev[pg - 1]) pm->aPrev[pg - 1] = sqlite3_malloc64((sqlite3_uint64)pm->pageSize);
    if (pm->aPrev[pg - 1]) memcpy(pm->aPrev[pg - 1], pm->aLast[pg], (size_t)pm->pageSize);
  }
  for (pg = pm->sizeAfter + 1; pg <= pm->nPrev; pg++) { sqlite3_free(pm->aPrev[pg - 1]); pm->aPrev[pg - 1] = NULL; }
  pm->sizeThen = pm->sizeAfter;
  qfree(pm->pBuilding); pm->pBuilding = NULL;
  pthread_mutex_lock(&pm->mu); pm->nFinalized += pm->nBuilding; pm->nBuilding = 0; pthread_mutex_unlock(&pm->mu);
  pm->nDirty = 0;
}

void hpplite_pagemode_failed(HpplitePageMode *pm) {
  unsigned k, pg;
  QCommit *tail;
  if (!pm || !pm->pBuilding) return;
  /* the root back to the last finalized state: the dirty pages' old images, the old size */
  for (k = 0; k < pm->nDirty; k++) {
    pg = pm->aDirty[k];
    if (pg <= pm->sizeThen && pm->aPrev[pg - 1]) hpplite_pageroot_set(pm->pRoot, pg, pm->aPrev[pg - 1]);
  }
  hpplite_pageroot_truncate(pm->pRoot, pm->sizeThen);
  /* the commits back at the head of the queue, in order */
  for (tail = pm->pBuilding; tail->pNext; tail = tail->pNext) {}
  pthread_mutex_lock(&pm->mu);
  tail->pNext = pm->pHead; pm->pHead = pm->pBuilding; if (!pm->pTail) pm->pTail = tail;
  { QCommit *q; unsigned i; for (q = pm->pBuilding; q; q = q->pNext) { for (i = 0; i < q->n; i++) if (q->aPgno[i] > pm->maxPgQueued) pm->maxPgQueued = q->aPgno[i]; if (q == tail) break; } }
  pm->nQueued += pm->nBuilding; pm->nBuilding = 0;
  { QCommit *q; for (q = pm->pHead; q; q = q->pNext) { if (q == tail->pNext) break; pm->queuedBytes += (size_t)q->n * (size_t)pm->pageSize; } }
  pthread_mutex_unlock(&pm->mu);
  pm->pBuilding = NULL; pm->nDirty = 0;
}
