/* Page mode (#146). See pagemode.h. */
#include "hpplite_sqlite.h"
#include "pagemode.h"
#include "pages.h"
#include "walread.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

typedef struct QCommit QCommit;
struct QCommit {
  unsigned n;                 /* pages */
  unsigned *aPgno;
  /* the n images, back to back: in memory (aImg), or past a few MB queued at spillOff in the queue's file (#365:
  ** a backlog on disk, as SQLite and Litestream keep theirs), or - the file queued whole - in the posted file */
  unsigned char *aImg;
  int bSpilled, bPosted;
  sqlite3_int64 spillOff;
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
  size_t memBytes;            /* ...of which in memory: past HPPLITE_QUEUE_MEM a commit goes to the queue's file */
  char *zSpill; int fdSpill; sqlite3_int64 spillEnd;     /* <db>-hpplite-queue, emptied whenever the queue is (#365) */
  unsigned maxPgQueued;       /* the highest page number among queued commits: the build's arrays, without a scan of the queue */
  uint64_t nCommits;          /* captured since start */
  uint64_t nCaptured;         /* ...of which the sink's: the file as opened (a snapshot) is not one */
  uint64_t nFinalized;        /* in batches finalized */
  /* the flush thread's: the state after the last finalized batch */
  HpplitePageRoot *pRoot;
  /* each page's image then, in a file beside the database (#364: never the whole database in memory):
  ** <db>-hpplite-posted, page n at (n - 1) * pageSize, and a bit a page saying it has one there (0: beyond the
  ** file, or none known - it goes whole) */
  char *zPosted; int fdPosted;
  unsigned char *aHas; unsigned nPrev;        /* room for nPrev pages' bits */
  unsigned sizeThen;                          /* the file's size in pages then */
  /* the batch in flight */
  QCommit *pBuilding; unsigned nBuilding;
  unsigned *aDirty; unsigned nDirty;          /* its dirty pages, each with its last image in the building commits */
  QCommit **aLastQ; unsigned *aLastI;          /* ...each page's last image: that commit's page i */
  unsigned char *pCur;                          /* a page: a last image read from a file */
  unsigned sizeAfter;
  int bWatch;
  char *zName;                /* on the next batch built: the database's name (#160) */
  unsigned takeExactly;       /* a build of exactly this many commits (0: by the budget) */
  unsigned maxRecords;        /* ...and at most this many records: one piece (0: whole) */
  unsigned pieceOff;          /* a commit cut over batches (#152): records of it already posted, in page order */
  unsigned pieceNo;           /* ...and pieces posted */
  unsigned pieceMore;         /* the batch being built is a piece with more after it */
  unsigned pieceEmitted;      /* pages taken in the piece being built (a record each, but those unchanged) */
  int pieceRooted;            /* the piece's commit's pages are in the root (the first piece set them; a failure undoes it) */
};

/* ------------------------------------------------- the posted images (#364) */
static int prevHas(HpplitePageMode *pm, unsigned pg) {
  return pg >= 1 && pg <= pm->nPrev && ((pm->aHas[(pg - 1) >> 3] >> ((pg - 1) & 7)) & 1);
}
static void prevBit(HpplitePageMode *pm, unsigned pg, int on) {
  if (pg < 1 || pg > pm->nPrev) return;
  if (on) pm->aHas[(pg - 1) >> 3] |= (unsigned char)(1u << ((pg - 1) & 7));
  else pm->aHas[(pg - 1) >> 3] &= (unsigned char)~(1u << ((pg - 1) & 7));
}
/* page pg as last posted into out: 1, or 0 for none */
static int prevRead(HpplitePageMode *pm, unsigned pg, unsigned char *out) {
  size_t ps = (size_t)pm->pageSize, got = 0;
  if (!prevHas(pm, pg) || pm->fdPosted < 0) return 0;
  while (got < ps) {
    ssize_t k = pread(pm->fdPosted, out + got, ps - got, (off_t)(pg - 1) * (off_t)ps + (off_t)got);
    if (k <= 0) { prevBit(pm, pg, 0); return 0; }               /* unreadable: the page goes whole */
    got += (size_t)k;
  }
  return 1;
}
/* page pg's image as posted now; a page that cannot be written has none, so it goes whole next time */
static void prevWrite(HpplitePageMode *pm, unsigned pg, const unsigned char *page) {
  size_t ps = (size_t)pm->pageSize, put = 0;
  if (pg < 1 || pg > pm->nPrev || ps == 0) return;
  if (pm->fdPosted < 0 && pm->zPosted) {
    struct stat st;
    mode_t mode = pm->zPath && stat(pm->zPath, &st) == 0 ? (st.st_mode & 0666) : 0600;     /* as private as the database */
    pm->fdPosted = open(pm->zPosted, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  }
  if (pm->fdPosted < 0) { prevBit(pm, pg, 0); return; }
  while (put < ps) {
    ssize_t k = pwrite(pm->fdPosted, page + put, ps - put, (off_t)(pg - 1) * (off_t)ps + (off_t)put);
    if (k <= 0) { prevBit(pm, pg, 0); return; }
    put += (size_t)k;
  }
  prevBit(pm, pg, 1);
}
static const unsigned char *lastImg(HpplitePageMode *pm, unsigned pg, unsigned char *buf);
/* page pg as just posted: its last image in the batch, into the posted file - already there for the file queued whole */
static void prevPosted(HpplitePageMode *pm, unsigned pg) {
  const unsigned char *img;
  if (pm->aLastQ[pg] && pm->aLastQ[pg]->bPosted) { prevBit(pm, pg, 1); return; }
  img = lastImg(pm, pg, pm->pCur);
  if (img) prevWrite(pm, pg, img); else prevBit(pm, pg, 0);   /* unreadable: it goes whole next time */
}

/* the file cut to n pages: none beyond */
static void prevCut(HpplitePageMode *pm, unsigned n) {
  unsigned pg;
  for (pg = n + 1; pg <= pm->nPrev; pg++) prevBit(pm, pg, 0);
  if (pm->fdPosted >= 0 && pm->pageSize > 0 && ftruncate(pm->fdPosted, (off_t)n * (off_t)pm->pageSize) != 0) {}
}

/* images a queue keeps in memory, the rest in its file (#365): HPPLITE_QUEUE_MEM bytes (default 32 MiB) - a constant,
** whatever the database's size or the outage's length; a write-heavy app's ordinary backlog fits in it */
static size_t queueMem(void) {
  static size_t n = 0;
  if (!n) { const char *z = getenv("HPPLITE_QUEUE_MEM"); long long v = z ? atoll(z) : 0; n = v > 0 ? (size_t)v : 32u * 1024 * 1024; }
  return n;
}

static void qfree(QCommit *q) { while (q) { QCommit *n = q->pNext; sqlite3_free(q->aPgno); sqlite3_free(q->aImg); sqlite3_free(q); q = n; } }
/* the commits freed, and their memory off the count; the queue's file emptied when nothing is queued or building */
static void qrelease(HpplitePageMode *pm, QCommit *q) {
  size_t mem = 0;
  QCommit *p;
  for (p = q; p; p = p->pNext) if (p->aImg) mem += (size_t)p->n * (size_t)pm->pageSize;
  qfree(q);
  pthread_mutex_lock(&pm->mu);
  pm->memBytes = pm->memBytes > mem ? pm->memBytes - mem : 0;
  pthread_mutex_unlock(&pm->mu);
}
static void spillReset(HpplitePageMode *pm) {
  pthread_mutex_lock(&pm->mu);
  if (!pm->pHead && !pm->pBuilding && pm->spillEnd > 0) {
    pm->spillEnd = 0;
    if (pm->fdSpill >= 0 && ftruncate(pm->fdSpill, 0) != 0) {}
  }
  pthread_mutex_unlock(&pm->mu);
}
/* the image of page i of q: in memory, or read into buf from where it lies */
static const unsigned char *qImg(HpplitePageMode *pm, QCommit *q, unsigned i, unsigned char *buf) {
  size_t ps = (size_t)pm->pageSize, got = 0;
  int fd;
  off_t off;
  if (q->aImg) return q->aImg + (size_t)i * ps;
  if (q->bPosted) { fd = pm->fdPosted; off = (off_t)(q->aPgno[i] - 1) * (off_t)ps; }
  else { fd = pm->fdSpill; off = (off_t)q->spillOff + (off_t)i * (off_t)ps; }
  if (fd < 0 || !buf) return NULL;
  while (got < ps) {
    ssize_t k = pread(fd, buf + got, ps - got, off + (off_t)got);
    if (k <= 0) return NULL;
    got += (size_t)k;
  }
  return buf;
}
/* page pg's last image in the batch being built */
static const unsigned char *lastImg(HpplitePageMode *pm, unsigned pg, unsigned char *buf) {
  return pm->aLastQ[pg] ? qImg(pm, pm->aLastQ[pg], pm->aLastI[pg], buf) : NULL;
}

/* the sink: the commit's pages, queued */
static void sink(void *pArg, const HppliteWalCommit *c) {
  HpplitePageMode *pm = pArg;
  QCommit *q = sqlite3_malloc64(sizeof(*q));
  size_t ps = (size_t)c->pageSize, size = (size_t)c->nPages * ps;
  unsigned i;
  if (!q) return;
  memset(q, 0, sizeof(*q));
  q->n = c->nPages; q->nTrunc = c->nTruncate; q->salt1 = c->salt1; q->salt2 = c->salt2;
  q->aPgno = sqlite3_malloc64((sqlite3_uint64)c->nPages * sizeof(unsigned));
  if (!q->aPgno) { qfree(q); return; }
  for (i = 0; i < c->nPages; i++) q->aPgno[i] = c->aPgno[i];
  pthread_mutex_lock(&pm->mu);
  if (pm->memBytes + size > queueMem() && pm->zSpill) {
    /* a backlog: to the queue's file, written while the queue is held so it is not emptied under the write (#365) */
    unsigned char *page = sqlite3_malloc64(ps);
    if (pm->fdSpill < 0) {
      struct stat st;
      mode_t mode = pm->zPath && stat(pm->zPath, &st) == 0 ? (st.st_mode & 0666) : 0600;
      pm->fdSpill = open(pm->zSpill, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    }
    for (i = 0; page && pm->fdSpill >= 0 && i < c->nPages; i++) {
      off_t off = (off_t)pm->spillEnd + (off_t)i * (off_t)ps;
      if (hpplite_walcommit_page(c, i, page) != 0 || pwrite(pm->fdSpill, page, ps, off) != (ssize_t)ps) break;
    }
    sqlite3_free(page);
    if (page && pm->fdSpill >= 0 && i == c->nPages) { q->bSpilled = 1; q->spillOff = pm->spillEnd; pm->spillEnd += (sqlite3_int64)size; }
  }
  if (!q->bSpilled) {
    /* in memory: a short queue, or the file failing; read with the queue let go (the reader's lock keeps the frames) */
    pm->memBytes += size;
    pthread_mutex_unlock(&pm->mu);
    q->aImg = sqlite3_malloc64(size ? size : 1);
    if (!q->aImg || hpplite_walcommit_pages(c, q->aImg) != 0) {
      pthread_mutex_lock(&pm->mu); pm->memBytes -= size; pthread_mutex_unlock(&pm->mu);
      qfree(q); return;
    }
    pthread_mutex_lock(&pm->mu);
  }
  for (i = 0; i < c->nPages; i++) if (c->aPgno[i] > pm->maxPgQueued) pm->maxPgQueued = c->aPgno[i];
  if (pm->pageSize == 0) pm->pageSize = c->pageSize;
  if (pm->pTail) pm->pTail->pNext = q; else pm->pHead = q;
  pm->pTail = q; pm->nQueued++; pm->queuedBytes += (size_t)c->nPages * (size_t)c->pageSize; pm->nCommits++; pm->nCaptured++;
  pthread_mutex_unlock(&pm->mu);
}

static int ensurePrev(HpplitePageMode *pm, unsigned n) {
  if (n <= pm->nPrev) return SQLITE_OK;
  { unsigned cap = pm->nPrev ? pm->nPrev : 256; while (cap < n) cap *= 2;
    unsigned char *a = sqlite3_realloc64(pm->aHas, (sqlite3_uint64)cap / 8 + 1);
    if (!a) return SQLITE_NOMEM;
    memset(a + pm->nPrev / 8, 0, (size_t)(cap / 8 + 1 - pm->nPrev / 8));
    pm->aHas = a; pm->nPrev = cap; }
  return SQLITE_OK;
}

HpplitePageMode *hpplite_pagemode_start(const char *zPath, int bWatch, unsigned startFrame) {
  HpplitePageMode *pm = sqlite3_malloc64(sizeof(*pm));
  HppliteDbFile *f;
  unsigned char hdr[100];
  unsigned i, n;
  if (!pm) return NULL;
  memset(pm, 0, sizeof(*pm));
  pthread_mutex_init(&pm->mu, NULL);
  pm->zPath = sqlite3_mprintf("%s", zPath);
  pm->zPosted = sqlite3_mprintf("%s-hpplite-posted", zPath);
  pm->fdPosted = -1;
  pm->zSpill = sqlite3_mprintf("%s-hpplite-queue", zPath);
  pm->fdSpill = -1;
  /* the file as it is: every page's image, the root (read through the VFS:
  ** a plain fclose would drop the node's own SHARED lock, somaro#116) */
  f = hpplite_dbfile_open(zPath);
  if (f && hpplite_dbfile_read(f, hdr, 100, 0) == 100) {
    pm->pageSize = (hdr[16] << 8) | hdr[17];
    if (pm->pageSize == 1) pm->pageSize = 65536;
    n = ((unsigned)hdr[28] << 24) | ((unsigned)hdr[29] << 16) | ((unsigned)hdr[30] << 8) | hdr[31];
    pm->pRoot = hpplite_pageroot_new(pm->pageSize);
    if (pm->pRoot && ensurePrev(pm, n) == SQLITE_OK) {
      unsigned char *img = sqlite3_malloc64((sqlite3_uint64)pm->pageSize);
      for (i = 1; img && i <= n; i++) {
        if (i == hpplite_lock_page(pm->pageSize)) continue;     /* never written, never content (#363) */
        if (hpplite_dbfile_read(f, img, (size_t)pm->pageSize, (int64_t)(i - 1) * pm->pageSize) != (size_t)pm->pageSize) break;
        prevWrite(pm, i, img);                                  /* to the posted file, a page at a time (#364) */
        hpplite_pageroot_set(pm->pRoot, i, img);
      }
      pm->sizeThen = img ? i - 1 : 0;
      sqlite3_free(img);
    }
  } else {
    pm->pRoot = hpplite_pageroot_new(0);          /* an empty file: the first commit says the page size */
  }
  hpplite_dbfile_close(f);
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
  unsigned i, j, n, lock;
  if (!pm || !pm->pRoot || pm->pageSize <= 0) return SQLITE_MISUSE;
  n = pm->sizeThen;
  if (n == 0) return SQLITE_OK;
  lock = hpplite_lock_page(pm->pageSize);
  q = sqlite3_malloc64(sizeof(*q));
  if (!q) return SQLITE_NOMEM;
  memset(q, 0, sizeof(*q));
  q->n = lock <= n ? n - 1 : n; q->nTrunc = n;               /* every page but the lock-byte page (#363) */
  q->bPosted = 1;                                              /* its images are the posted file's: no copy (#364) */
  q->aPgno = sqlite3_malloc64((sqlite3_uint64)n * sizeof(unsigned));
  if (!q->aPgno) { qfree(q); return SQLITE_NOMEM; }
  for (i = 0, j = 0; i < n; i++) {
    if (i + 1 == lock) continue;
    if (!prevHas(pm, i + 1)) { qfree(q); return SQLITE_IOERR; }
    q->aPgno[j] = i + 1;
    prevBit(pm, i + 1, 0);                                     /* never shipped: the batch carries them whole */
    j++;
  }
  n = q->n;
  pthread_mutex_lock(&pm->mu);
  q->pNext = pm->pHead; pm->pHead = q; if (!pm->pTail) pm->pTail = q;
  pm->nQueued++; pm->queuedBytes += (size_t)n * (size_t)pm->pageSize; pm->nCommits++;
  if (n && q->aPgno[n - 1] > pm->maxPgQueued) pm->maxPgQueued = q->aPgno[n - 1];
  pthread_mutex_unlock(&pm->mu);
  return SQLITE_OK;
}

int hpplite_pagemode_resync(HpplitePageMode *pm) {
  HppliteDbFile *f;
  unsigned char hdr[100];
  unsigned i, n;
  int pageSize;
  if (!pm || !pm->pRoot || !pm->zPath || pm->pageSize <= 0 || hpplite_pagemode_pending(pm)) return SQLITE_MISUSE;
  f = hpplite_dbfile_open(pm->zPath);
  if (!f) return SQLITE_CANTOPEN;
  if (hpplite_dbfile_read(f, hdr, 100, 0) != 100) { hpplite_dbfile_close(f); return SQLITE_CORRUPT; }
  pageSize = (hdr[16] << 8) | hdr[17];
  if (pageSize == 1) pageSize = 65536;
  n = ((unsigned)hdr[28] << 24) | ((unsigned)hdr[29] << 16) | ((unsigned)hdr[30] << 8) | hdr[31];
  if (pageSize != pm->pageSize || ensurePrev(pm, n) != SQLITE_OK) { hpplite_dbfile_close(f); return SQLITE_MISMATCH; }
  prevCut(pm, 0);
  {
    unsigned char *img = sqlite3_malloc64((sqlite3_uint64)pageSize);
    if (!img) { hpplite_dbfile_close(f); return SQLITE_NOMEM; }
    for (i = 1; i <= n; i++) {
      if (i == hpplite_lock_page(pageSize)) continue;           /* never written, never content (#363) */
      if (hpplite_dbfile_read(f, img, (size_t)pageSize, (int64_t)(i - 1) * pageSize) != (size_t)pageSize) break;
      prevWrite(pm, i, img);
    }
    sqlite3_free(img);
  }
  hpplite_dbfile_close(f);
  pm->sizeThen = i - 1;
  return hpplite_pagemode_snapshot(pm);
}

int hpplite_pagemode_applied(HpplitePageMode *pm, unsigned pgno, const unsigned char *page) {
  if (!pm || !pm->pRoot) return SQLITE_MISUSE;
  if (pm->pageSize == 0 && pgno == 1) { pm->pageSize = (page[16] << 8) | page[17]; if (pm->pageSize == 1) pm->pageSize = 65536; hpplite_pageroot_free(pm->pRoot); pm->pRoot = hpplite_pageroot_new(pm->pageSize); }
  /* the page as written is also what this file's next commits are diffed against: a sequencer that rebuilt its file
  ** and writes on in the same run must diff against the rebuilt pages, as every replica of the history has them - left
  ** at the file as it was before the rebuild, its first diffs replayed elsewhere to another page (somaro's restore #3) */
  if (pgno >= 1 && pm->pageSize > 0 && ensurePrev(pm, pgno) == SQLITE_OK) prevWrite(pm, pgno, page);
  return hpplite_pageroot_set(pm->pRoot, pgno, page);
}
/* A replica rebuilt in another file and copied over this one (#166): its page tree is this file's now */
int hpplite_pagemode_take_root(HpplitePageMode *pm, HpplitePageMode *from) {
  HpplitePageRoot *r;
  if (!pm || !from || !from->pRoot) return SQLITE_MISUSE;
  r = pm->pRoot; pm->pRoot = from->pRoot; from->pRoot = r;
  { /* and its posted images: the copy's are this file's now (each removes the one it holds when it stops) */
    char *z = pm->zPosted; int fd = pm->fdPosted; unsigned char *a = pm->aHas; unsigned n = pm->nPrev;
    pm->zPosted = from->zPosted; pm->fdPosted = from->fdPosted; pm->aHas = from->aHas; pm->nPrev = from->nPrev;
    from->zPosted = z; from->fdPosted = fd; from->aHas = a; from->nPrev = n; }
  pm->pageSize = from->pageSize;
  pm->sizeThen = from->sizeThen;
  return SQLITE_OK;
}

int hpplite_pagemode_applied_size(HpplitePageMode *pm, unsigned nPages) {
  unsigned pg;
  if (!pm || !pm->pRoot) return SQLITE_MISUSE;
  pm->sizeThen = nPages;
  (void)pg;
  prevCut(pm, nPages);                                         /* cut off with the file */
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
  if (pm->fdSpill >= 0) close(pm->fdSpill);
  if (pm->zSpill) unlink(pm->zSpill);
  sqlite3_free(pm->zSpill);
  sqlite3_free(pm->pCur);
  (void)i;
  if (pm->fdPosted >= 0) close(pm->fdPosted);
  if (pm->zPosted) unlink(pm->zPosted);                       /* rebuilt from the file at every start */
  sqlite3_free(pm->zPosted);
  sqlite3_free(pm->aHas); sqlite3_free(pm->aDirty); sqlite3_free(pm->aLastQ); sqlite3_free(pm->aLastI);
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
  QCommit *q;
  pthread_mutex_lock(&pm->mu);
  q = pm->pHead; pm->pHead = pm->pTail = NULL; pm->nQueued = 0; pm->queuedBytes = 0; pm->maxPgQueued = 0;
  pthread_mutex_unlock(&pm->mu);
  qrelease(pm, q);
  spillReset(pm);
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

/* the image's batch, its pages not yet read: every page of the file but the lock-byte page (#363), each posted */
static int image_batch(HpplitePageMode *pm, HpplitePageBatch *b, unsigned *pLock) {
  unsigned i, n, lock, nRec = 0;
  if (!pm || !pm->pRoot || pm->pageSize <= 0 || pm->pBuilding) return SQLITE_MISUSE;
  n = pm->sizeThen;
  lock = hpplite_lock_page(pm->pageSize);
  for (i = 1; i <= n; i++) {
    if (i == lock) continue;
    if (!prevHas(pm, i)) return SQLITE_BUSY;                    /* a whole file queued, not yet finalized */
    nRec++;
  }
  memset(b, 0, sizeof(*b));
  b->pageSize = (unsigned)pm->pageSize; b->commitSize = n;
  b->firstCommit = b->lastCommit = pm->nFinalized;
  b->timestampMs = now_ms();
  b->nPages = nRec;
  hpplite_pageroot_get(pm->pRoot, b->root); b->crc = hpplite_pageroot_crc(pm->pRoot);
  *pLock = lock;
  return SQLITE_OK;
}

int hpplite_pagemode_image_begin(HpplitePageMode *pm, uint64_t *pLen, unsigned char root[32]) {
  HpplitePageBatch b;
  unsigned lock;
  int rc = image_batch(pm, &b, &lock);
  if (rc != SQLITE_OK) return rc;
  *pLen = (uint64_t)hpplite_pagebatch_size(&b);
  memcpy(root, b.root, 32);
  return SQLITE_OK;
}

#define IMAGE_RECS 64                                           /* records read from the posted file at a time */
int hpplite_pagemode_image_stream(HpplitePageMode *pm, int (*xOut)(void *, const unsigned char *, size_t), void *pArg) {
  HpplitePageBatch b;
  unsigned lock, i;
  size_t ps, rec, o, cap;
  unsigned char *buf;
  int rc = image_batch(pm, &b, &lock);
  if (rc != SQLITE_OK) return rc;
  ps = (size_t)pm->pageSize; rec = 6 + ps; cap = IMAGE_RECS * rec;
  buf = sqlite3_malloc64(cap);
  if (!buf) return SQLITE_NOMEM;
  o = hpplite_pagebatch_head(&b, buf);
  if (xOut(pArg, buf, o) != 0) { sqlite3_free(buf); return SQLITE_ABORT; }
  o = 0;
  for (i = 1; i <= b.commitSize; i++) {
    if (i == lock) continue;
    o += hpplite_pagebatch_rec(i, HPPLITE_PAGE_FULL, buf + o);
    if (!prevRead(pm, i, buf + o)) { sqlite3_free(buf); return SQLITE_IOERR; }
    o += ps;
    if (o + rec > cap) { if (xOut(pArg, buf, o) != 0) { sqlite3_free(buf); return SQLITE_ABORT; } o = 0; }
  }
  o += hpplite_pagebatch_tail(&b, buf + o);                    /* 40 bytes: within a record's room */
  rc = xOut(pArg, buf, o) != 0 ? SQLITE_ABORT : SQLITE_OK;
  sqlite3_free(buf);
  return rc;
}

int hpplite_pagemode_image(HpplitePageMode *pm, unsigned char **ppBytes, size_t *pLen, unsigned char root[32]) {
  HpplitePageBatch b;
  HpplitePageRec *recs;
  unsigned i, n, nRec = 0, lock;
  *ppBytes = NULL; *pLen = 0;
  if (!pm || !pm->pRoot || pm->pageSize <= 0 || pm->pBuilding) return SQLITE_MISUSE;
  n = pm->sizeThen;
  lock = hpplite_lock_page(pm->pageSize);                    /* no image of it: never written, never content (#363) */
  for (i = 0; i < n; i++) if (i + 1 != lock && !prevHas(pm, i + 1)) return SQLITE_BUSY;      /* a whole file queued, not yet finalized */
  recs = sqlite3_malloc64((sqlite3_uint64)(n ? n : 1) * sizeof(*recs));
  if (!recs) return SQLITE_NOMEM;
  for (i = 0; i < n; i++) if (i + 1 != lock) { recs[nRec].pgno = i + 1; recs[nRec].flags = HPPLITE_PAGE_FULL; recs[nRec].body = NULL; nRec++; }   /* filled from the posted file, in place */
  memset(&b, 0, sizeof b);
  b.pageSize = (unsigned)pm->pageSize; b.commitSize = n;
  b.firstCommit = b.lastCommit = pm->nFinalized;
  b.timestampMs = now_ms();
  b.nPages = nRec; b.aPage = recs;
  hpplite_pageroot_get(pm->pRoot, b.root); b.crc = hpplite_pageroot_crc(pm->pRoot);
  memcpy(root, b.root, 32);
  *ppBytes = hpplite_pagebatch_encode(&b, pLen);
  if (*ppBytes) {
    for (i = 0; i < nRec; i++) if (!prevRead(pm, recs[i].pgno, *ppBytes + hpplite_pagebatch_body_at(i, b.pageSize))) break;
    if (i < nRec) { sqlite3_free(*ppBytes); *ppBytes = NULL; *pLen = 0; sqlite3_free(recs); return SQLITE_IOERR; }
  }
  sqlite3_free(recs);
  return *ppBytes ? SQLITE_OK : SQLITE_NOMEM;
}


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
  pm->aLastQ = sqlite3_realloc64(pm->aLastQ, (sqlite3_uint64)(maxPg + 1) * sizeof(QCommit *));
  pm->aLastI = sqlite3_realloc64(pm->aLastI, (sqlite3_uint64)(maxPg + 1) * sizeof(unsigned));
  if (!pm->pCur) pm->pCur = sqlite3_malloc64((sqlite3_uint64)ps);
  if (!pm->aDirty || !pm->aLastQ || !pm->aLastI || !pm->pCur) { sqlite3_free(seen); hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  pm->nDirty = 0;
  for (q = pm->pBuilding; q; q = q->pNext) {
    for (i = 0; i < q->n; i++) {
      pg = q->aPgno[i];
      if (!seen[pg]) { seen[pg] = 1; pm->aDirty[pm->nDirty++] = pg; }
      pm->aLastQ[pg] = q; pm->aLastI[pg] = i;
    }
    pm->sizeAfter = q->nTrunc;
  }
  sqlite3_free(seen);
  /* the root after: dirty pages within the file set, the file cut to its size */
  if (ensurePrev(pm, pm->sizeAfter > maxPg ? pm->sizeAfter : maxPg) != SQLITE_OK) { hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  { /* room for the records this build can take: a piece takes at most its cap, not the whole commit (#364) */
    unsigned capRec = pm->maxRecords ? pm->maxRecords : (unsigned)(budget < pm->nDirty ? budget : pm->nDirty);
    unsigned nRoom = nTake == 1 && (pm->pieceOff > 0 || pm->nDirty > capRec) && capRec < pm->nDirty ? capRec : pm->nDirty;
    recs = sqlite3_malloc64((sqlite3_uint64)(nRoom ? nRoom : 1) * sizeof(HpplitePageRec));
    bodies = sqlite3_malloc64((sqlite3_uint64)(nRoom ? nRoom : 1) * ps + ps);       /* and one page more: the old image read */
  }
  if (!recs || !bodies) { sqlite3_free(recs); sqlite3_free(bodies); hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  /* records in page order */
  seen = sqlite3_malloc64(maxPg + 1);
  if (!seen) { sqlite3_free(recs); sqlite3_free(bodies); hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  memset(seen, 0, maxPg + 1);
  { unsigned k; for (k = 0; k < pm->nDirty; k++) seen[pm->aDirty[k]] = 1; }
  /* one commit with more pages than fit is cut into pieces (#152): this
  ** build takes the records after those already posted, as many as fit;
  ** a replica holds its lock from the first piece to the last */
  { unsigned cap = pm->maxRecords ? pm->maxRecords : (unsigned)budget, inOrder = 0, skipped = 0, highest = 0, used = 0;
    int piece = nTake == 1 && (pm->pieceOff > 0 || pm->nDirty > cap);
    pm->pieceMore = 0; pm->pieceEmitted = 0;
    for (pg = 1; pg <= maxPg; pg++) {
      if (!seen[pg] || pg > pm->sizeAfter) continue;
      inOrder++;
      if (piece && skipped < pm->pieceOff) { skipped++; continue; }           /* posted in an earlier piece */
      if (piece && nRec >= cap) { pm->pieceMore = 1; break; }                /* the next piece's */
      used++; highest = pg;
      /* a page written as it was last sent (a backup into the file rewrites them all, #345) needs no record: every
      ** replica has it, and the root's leaf is already its. Taken by this piece all the same, so the pieces' positions
      ** hold; the cap stays on records, as an adoption rebuilds a piece from its record count */
      { unsigned char *body = bodies + (size_t)nRec * ps, *old = body + ps;   /* the old image read past the last body */
        const unsigned char *cur = lastImg(pm, pg, pm->pCur);
        if (!cur) { sqlite3_free(recs); sqlite3_free(bodies); sqlite3_free(seen); hpplite_pagemode_failed(pm); return SQLITE_IOERR; }
        if (!prevRead(pm, pg, old)) old = NULL;
        if (old && memcmp(cur, old, ps) == 0) continue;
        if (old) { unsigned j; for (j = 0; j < ps; j++) body[j] = cur[j] ^ old[j]; recs[nRec].flags = HPPLITE_PAGE_DIFF; }
        else { memcpy(body, cur, ps); recs[nRec].flags = HPPLITE_PAGE_FULL; }
        recs[nRec].pgno = pg; recs[nRec].body = body; nRec++;
        if (!piece) hpplite_pageroot_set(pm->pRoot, pg, cur); }
    }
    (void)inOrder;
    pm->pieceEmitted = used;
    memset(&b, 0, sizeof b);
    if (piece) {
      /* a piece's size: the file as it was, or as far as its pages reach; the
      ** last piece cuts to the commit's. Every piece carries the root of the
      ** whole commit (a replica checks it at the last piece: half a commit has
      ** no root of its own, and a file shipped whole after a crash is one commit
      ** the replica's images cannot stand in for), so the kept root is the
      ** commit's final state from the first piece on */
      unsigned sz = pm->pieceMore ? (highest > pm->sizeThen ? highest : pm->sizeThen) : pm->sizeAfter;
      b.piece = pm->pieceNo + 1; b.bMore = pm->pieceMore;
      b.commitSize = sz;
      if (!pm->pieceRooted) {
        /* once for the commit, not again at every piece (each was a pass over the whole commit) */
        for (pg = 1; pg <= maxPg; pg++) if (seen[pg] && pg <= pm->sizeAfter) {
          const unsigned char *cur = lastImg(pm, pg, pm->pCur);
          if (!cur) { sqlite3_free(recs); sqlite3_free(bodies); sqlite3_free(seen); hpplite_pagemode_failed(pm); return SQLITE_IOERR; }
          hpplite_pageroot_set(pm->pRoot, pg, cur);
        }
        hpplite_pageroot_truncate(pm->pRoot, pm->sizeAfter);
        pm->pieceRooted = 1;
      }
    } else {
      b.commitSize = pm->sizeAfter;
      hpplite_pageroot_truncate(pm->pRoot, pm->sizeAfter);
    }
    sqlite3_free(seen);
  }
  b.pageSize = ps;
  b.firstCommit = pm->nFinalized + 1; b.lastCommit = pm->nFinalized + nTake;
  b.salt1 = last->salt1; b.salt2 = last->salt2; b.timestampMs = now_ms();
  b.nPages = nRec; b.aPage = recs;
  b.zName = pm->zName; b.nName = pm->zName ? (unsigned)strlen(pm->zName) : 0;
  hpplite_pageroot_get(pm->pRoot, b.root); b.crc = hpplite_pageroot_crc(pm->pRoot);
  memcpy(root, b.root, 32);
  *ppBytes = hpplite_pagebatch_encode(&b, pLen);
  sqlite3_free(recs); sqlite3_free(bodies);
  if (!*ppBytes) { hpplite_pagemode_failed(pm); return SQLITE_NOMEM; }
  if (pm->pieceMore) *pMore = 1;                   /* the rest of the commit follows */
  return SQLITE_OK;
}

int hpplite_pagemode_build_n(HpplitePageMode *pm, unsigned nCommits, unsigned maxRecords, unsigned char **ppBytes, size_t *pLen, unsigned char root[32], int *pMore) {
  int rc;
  if (!pm || nCommits == 0) return SQLITE_MISUSE;
  pm->takeExactly = nCommits; pm->maxRecords = maxRecords;
  rc = hpplite_pagemode_build(pm, (size_t)-1 / 2, ppBytes, pLen, root, pMore);
  pm->takeExactly = 0; pm->maxRecords = 0;
  return rc;
}
int hpplite_pagemode_piece_open(HpplitePageMode *pm) { return pm && pm->pieceOff > 0; }
int hpplite_pagemode_piece_more(HpplitePageMode *pm) { return pm && pm->pieceMore; }

void hpplite_pagemode_finalized(HpplitePageMode *pm) {
  unsigned k, pg;
  if (!pm || !pm->pBuilding) return;
  if (pm->pieceMore) {
    /* a piece: its pages are the replica's now; the commit stays, the next
    ** build takes the records after these */
    unsigned n = 0, skipped = 0, highest = 0;
    unsigned char *seen = sqlite3_malloc64((sqlite3_uint64)pm->nPrev + 1);
    if (!seen) return;
    memset(seen, 0, (size_t)pm->nPrev + 1);
    for (k = 0; k < pm->nDirty; k++) if (pm->aDirty[k] <= pm->nPrev) seen[pm->aDirty[k]] = 1;
    for (pg = 1; pg <= pm->nPrev; pg++) {
      if (!seen[pg] || pg > pm->sizeAfter) continue;
      if (skipped < pm->pieceOff) { skipped++; continue; }
      if (n >= pm->pieceEmitted) break;
      prevPosted(pm, pg);
      n++; highest = pg;
    }
    sqlite3_free(seen);
    if (highest > pm->sizeThen) pm->sizeThen = highest;
    pm->pieceOff += pm->pieceEmitted; pm->pieceNo++; pm->pieceMore = 0;
    /* the commit goes back to the head of the queue, to be taken again */
    pthread_mutex_lock(&pm->mu);
    pm->pBuilding->pNext = pm->pHead; pm->pHead = pm->pBuilding; if (!pm->pTail) pm->pTail = pm->pBuilding;
    { unsigned i; for (i = 0; i < pm->pBuilding->n; i++) if (pm->pBuilding->aPgno[i] > pm->maxPgQueued) pm->maxPgQueued = pm->pBuilding->aPgno[i]; }
    pm->nQueued += pm->nBuilding; pm->queuedBytes += (size_t)pm->pBuilding->n * (size_t)pm->pageSize; pm->nBuilding = 0;
    pthread_mutex_unlock(&pm->mu);
    pm->pBuilding = NULL; pm->nDirty = 0;
    return;
  }
  pm->pieceOff = 0; pm->pieceNo = 0; pm->pieceRooted = 0;
  for (k = 0; k < pm->nDirty; k++) {
    pg = pm->aDirty[k];
    if (pg > pm->sizeAfter) continue;
    prevPosted(pm, pg);
  }
  prevCut(pm, pm->sizeAfter);
  pm->sizeThen = pm->sizeAfter;
  qrelease(pm, pm->pBuilding); pm->pBuilding = NULL;
  pthread_mutex_lock(&pm->mu); pm->nFinalized += pm->nBuilding; pm->nBuilding = 0; pthread_mutex_unlock(&pm->mu);
  pm->nDirty = 0;
  spillReset(pm);
}

void hpplite_pagemode_failed(HpplitePageMode *pm) {
  unsigned k, pg;
  QCommit *tail;
  if (!pm || !pm->pBuilding) return;
  pm->pieceRooted = 0;                                          /* the root goes back below: a retry sets it again */
  /* the root back to the last finalized state: the dirty pages' old images, the old size
  ** (pages posted in an earlier piece of this commit are in the posted file as posted) */
  for (k = 0; k < pm->nDirty; k++) {
    pg = pm->aDirty[k];
    unsigned char *old = pm->pageSize > 0 ? sqlite3_malloc64((sqlite3_uint64)pm->pageSize) : NULL;
    if (old && pg <= pm->sizeThen && prevRead(pm, pg, old)) hpplite_pageroot_set(pm->pRoot, pg, old);
    sqlite3_free(old);                                          /* none: beyond the posted file, cut off below */
  }
  hpplite_pageroot_truncate(pm->pRoot, pm->sizeThen);
  pm->pieceMore = 0;
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
