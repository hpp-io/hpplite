/*
** HPPLite - page capture by reading the WAL file. See walread.h.
**
** The WAL (https://sqlite.org/fileformat.html#walformat): a 32-byte header
** (magic 0x377f0682 or 0x377f0683 for the checksum's byte order, version,
** page size, checkpoint sequence, salt1, salt2, checksum), then frames of a
** 24-byte header (page number, the database size in pages when the frame
** commits, salt1, salt2, checksum) and a page. A frame is valid when its
** salts are the header's and its checksum continues the chain from the
** header through every frame before it.
*/
#include "hpplite_sqlite.h"
#include "walread.h"
#include <pthread.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#define WAL_HDR 32
#define FRAME_HDR 24
#define READ_FRAMES 64                              /* frames per read */

typedef struct Reader Reader;
struct Reader {
  char *zPath, *zWal;
  HppliteWalSink xSink;
  void *pArg;
  pthread_mutex_t mu;
  int fd;
  int pageSize;
  int bigEndian;                                    /* the checksum's word order */
  unsigned salt1, salt2;                            /* the generation the cursor is in (0,0: none yet) */
  unsigned next;                                    /* the next frame to read, 1-based */
  unsigned s1, s2;                                  /* the checksum chain after frame next-1 */
  unsigned skip;                                    /* frames the file held when the watch began: read for the chain, not delivered */
  /* the transaction being collected */
  unsigned char **aImg; unsigned *aPgno; unsigned nTxn, nTxnAlloc;
  unsigned char *buf;                               /* READ_FRAMES frames */
  Reader *pNext;
};
static pthread_mutex_t gMutex = PTHREAD_MUTEX_INITIALIZER;
static Reader *gReaders;

static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static unsigned le32(const unsigned char *p) { return ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0]; }

/* SQLite's walChecksumBytes: 8 bytes at a time, the two words in the header's byte order */
static void cksum(int bigEndian, const unsigned char *p, size_t n, unsigned *s1, unsigned *s2) {
  unsigned a = *s1, b = *s2;
  size_t i;
  for (i = 0; i + 8 <= n; i += 8) {
    unsigned x0 = bigEndian ? be32(p + i) : le32(p + i), x1 = bigEndian ? be32(p + i + 4) : le32(p + i + 4);
    a += x0 + b;
    b += x1 + a;
  }
  *s1 = a; *s2 = b;
}

static Reader *find(const char *zPath) {
  Reader *r;
  for (r = gReaders; r; r = r->pNext) if (strcmp(r->zPath, zPath) == 0) return r;
  return NULL;
}

static void txnFree(Reader *r) {
  unsigned i;
  for (i = 0; i < r->nTxn; i++) sqlite3_free(r->aImg[i]);
  r->nTxn = 0;
}
static int txnAdd(Reader *r, unsigned pg, const unsigned char *img) {
  unsigned i;
  for (i = 0; i < r->nTxn; i++) if (r->aPgno[i] == pg) { memcpy(r->aImg[i], img, (size_t)r->pageSize); return SQLITE_OK; }
  if (r->nTxn == r->nTxnAlloc) {
    unsigned n = r->nTxnAlloc ? r->nTxnAlloc * 2 : 32;
    unsigned char **aImg = sqlite3_realloc64(r->aImg, (sqlite3_uint64)n * sizeof(*aImg));
    unsigned *aPgno = sqlite3_realloc64(r->aPgno, (sqlite3_uint64)n * sizeof(*aPgno));
    if (!aImg || !aPgno) { sqlite3_free(aImg); sqlite3_free(aPgno); return SQLITE_NOMEM; }
    r->aImg = aImg; r->aPgno = aPgno; r->nTxnAlloc = n;
  }
  r->aImg[r->nTxn] = sqlite3_malloc64((sqlite3_uint64)r->pageSize);
  if (!r->aImg[r->nTxn]) return SQLITE_NOMEM;
  memcpy(r->aImg[r->nTxn], img, (size_t)r->pageSize);
  r->aPgno[r->nTxn] = pg;
  r->nTxn++;
  return SQLITE_OK;
}
static void txnCommit(Reader *r, unsigned frame, unsigned nTruncate) {
  HppliteWalCommit c;
  c.zPath = r->zPath; c.pageSize = r->pageSize; c.nPages = r->nTxn; c.aPgno = r->aPgno;
  c.aImg = (const unsigned char *const *)r->aImg; c.nTruncate = nTruncate;
  c.salt1 = r->salt1; c.salt2 = r->salt2; c.frame = frame;
  if (r->xSink && r->nTxn && frame > r->skip) r->xSink(r->pArg, &c);
  txnFree(r);
}

static int readAt(int fd, unsigned char *p, size_t n, off_t off) {
  size_t got = 0;
  while (got < n) {
    ssize_t k = pread(fd, p + got, n - got, off + (off_t)got);
    if (k < 0) return -1;
    if (k == 0) break;
    got += (size_t)k;
  }
  return (int)got;
}

/* with the reader's lock: the frames committed since the cursor, to mxFrame (0: the file's end) */
/* the committing connection's own frames, with the count SQLite handed its
** hook: no stat, no header read, no checksum (the frames are this process's,
** written a moment ago); the salts are checked, and anything unexpected
** (a restart, a short read) falls back to the full look */
static int pollOwnLocked(Reader *r, unsigned mxFrame, int *pDone) {
  unsigned frameSz = FRAME_HDR + (unsigned)r->pageSize, f = r->next, i, n;
  int delivered = 0, got;
  *pDone = 0;
  if (r->fd < 0 || r->next == 0 || mxFrame < r->next || !r->buf) return 0;
  while (f <= mxFrame) {
    n = mxFrame - f + 1;
    if (n > READ_FRAMES) n = READ_FRAMES;
    got = readAt(r->fd, r->buf, (size_t)n * frameSz, (off_t)WAL_HDR + (off_t)(f - 1) * frameSz);
    if (got < (int)frameSz) { txnFree(r); return 0; }
    n = (unsigned)got / frameSz;
    for (i = 0; i < n; i++, f++) {
      const unsigned char *fr = r->buf + (size_t)i * frameSz;
      unsigned pg = be32(fr), nTrunc = be32(fr + 4);
      if (be32(fr + 8) != r->salt1 || be32(fr + 12) != r->salt2) { txnFree(r); return 0; }   /* not this generation: the full look */
      if (txnAdd(r, pg, fr + FRAME_HDR) != SQLITE_OK) { txnFree(r); return 0; }
      if (nTrunc) { txnCommit(r, f, nTrunc); r->next = f + 1; delivered++; }
    }
  }
  txnFree(r);                                       /* a transaction in flight past mxFrame: next time */
  *pDone = 1;
  return delivered;
}

static int pollLocked(Reader *r, unsigned mxFrame) {
  unsigned char hdr[WAL_HDR];
  struct stat st;
  unsigned magic, pageSize, salt1, salt2, frameSz, mx, f;
  unsigned hs1 = 0, hs2 = 0;
  int delivered = 0;
  if (mxFrame) {
    int done = 0;
    delivered = pollOwnLocked(r, mxFrame, &done);
    if (done) {
      /* the checksum chain is not advanced here: the next full look rebuilds
      ** it from the last commit frame's stored checksum */
      return delivered;
    }
  }
  if (r->fd < 0) {
    r->fd = open(r->zWal, O_RDONLY | O_CLOEXEC);
    if (r->fd < 0) return 0;                        /* no WAL yet (or deleted: a switch out of WAL mode) */
  }
  if (fstat(r->fd, &st) != 0) { close(r->fd); r->fd = -1; return 0; }
  if (st.st_nlink == 0) { close(r->fd); r->fd = -1; return 0; }   /* deleted and recreated: next poll reopens */
  if (st.st_size < WAL_HDR) return 0;
  if (readAt(r->fd, hdr, WAL_HDR, 0) != WAL_HDR) return 0;
  magic = be32(hdr);
  if (magic != 0x377f0682u && magic != 0x377f0683u) return 0;
  pageSize = be32(hdr + 8); salt1 = be32(hdr + 16); salt2 = be32(hdr + 20);
  if (pageSize < 512 || pageSize > 65536 || (pageSize & (pageSize - 1))) return 0;
  cksum(magic == 0x377f0683u, hdr, 24, &hs1, &hs2);
  if (hs1 != be32(hdr + 24) || hs2 != be32(hdr + 28)) return 0;   /* a header being written */
  if (salt1 != r->salt1 || salt2 != r->salt2 || (int)pageSize != r->pageSize) {
    /* a new generation: a WAL restarted after a checkpoint, or the first look */
    if (r->next) r->skip = 0;                       /* the frames to skip were the first generation's */
    r->salt1 = salt1; r->salt2 = salt2; r->pageSize = (int)pageSize; r->bigEndian = magic == 0x377f0683u;
    r->next = 1; r->s1 = hs1; r->s2 = hs2;
    txnFree(r);
    sqlite3_free(r->buf); r->buf = NULL;
  } else if (r->next > 1) {
    /* the chain through frame next-1, as that frame stores it (the fast path
    ** moves the cursor without computing it) */
    unsigned char fh[FRAME_HDR];
    if (readAt(r->fd, fh, FRAME_HDR, (off_t)WAL_HDR + (off_t)(r->next - 2) * (FRAME_HDR + (off_t)r->pageSize)) == FRAME_HDR
        && be32(fh + 8) == salt1 && be32(fh + 12) == salt2) { r->s1 = be32(fh + 16); r->s2 = be32(fh + 20); }
  }
  if (!r->buf) { r->buf = sqlite3_malloc64((sqlite3_uint64)READ_FRAMES * (FRAME_HDR + (sqlite3_uint64)r->pageSize)); if (!r->buf) return SQLITE_NOMEM; }
  frameSz = FRAME_HDR + pageSize;
  mx = (unsigned)((st.st_size - WAL_HDR) / frameSz);
  if (mxFrame && mxFrame < mx) mx = mxFrame;
  f = r->next;
  {
    unsigned a = r->s1, b = r->s2;                  /* the chain through the frames scanned so far */
    int stop = 0;
    while (f <= mx && !stop) {
      unsigned n = mx - f + 1, i;
      int got;
      if (n > READ_FRAMES) n = READ_FRAMES;
      got = readAt(r->fd, r->buf, (size_t)n * frameSz, (off_t)WAL_HDR + (off_t)(f - 1) * frameSz);
      if (got < (int)frameSz) break;
      n = (unsigned)got / frameSz;
      for (i = 0; i < n; i++, f++) {
        const unsigned char *fr = r->buf + (size_t)i * frameSz;
        unsigned pg = be32(fr), nTrunc = be32(fr + 4);
        if (be32(fr + 8) != salt1 || be32(fr + 12) != salt2) { stop = 1; break; }   /* an older generation's frame past the restart: the end */
        cksum(r->bigEndian, fr, 8, &a, &b);
        cksum(r->bigEndian, fr + FRAME_HDR, pageSize, &a, &b);
        if (a != be32(fr + 16) || b != be32(fr + 20)) { stop = 1; break; }         /* a frame still being written */
        if (txnAdd(r, pg, fr + FRAME_HDR) != SQLITE_OK) { txnFree(r); return SQLITE_NOMEM; }
        if (nTrunc) { txnCommit(r, f, nTrunc); r->next = f + 1; r->s1 = a; r->s2 = b; delivered++; }
      }
    }
  }
  /* frames after the last commit frame: a transaction in flight, read again
  ** next time from its first frame (the chain's state is the last commit's) */
  txnFree(r);
  return delivered;
}

int hpplite_walread_watch(const char *zPath, HppliteWalSink xSink, void *pArg, unsigned startFrame) {
  Reader *r;
  if (!zPath) return SQLITE_MISUSE;
  pthread_mutex_lock(&gMutex);
  r = find(zPath);
  if (!r) {
    r = sqlite3_malloc64(sizeof(*r));
    if (!r) { pthread_mutex_unlock(&gMutex); return SQLITE_NOMEM; }
    memset(r, 0, sizeof(*r));
    r->zPath = sqlite3_mprintf("%s", zPath);
    r->zWal = sqlite3_mprintf("%s-wal", zPath);
    r->fd = -1;
    pthread_mutex_init(&r->mu, NULL);
    r->pNext = gReaders; gReaders = r;
  }
  pthread_mutex_lock(&r->mu);
  r->xSink = xSink; r->pArg = pArg;
  if (r->next == 0 || startFrame) {
    /* a new reader, or a caller saying where the file stands: from there; a
    ** sink set again on a live reader goes on from its cursor */
    r->skip = startFrame;
    r->salt1 = r->salt2 = 0; r->next = 0;
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    txnFree(r);
  }
  pthread_mutex_unlock(&r->mu);
  pthread_mutex_unlock(&gMutex);
  return SQLITE_OK;
}

void hpplite_walread_unwatch(const char *zPath) {
  Reader *r;
  pthread_mutex_lock(&gMutex);
  r = zPath ? find(zPath) : NULL;
  if (r) {
    pthread_mutex_lock(&r->mu);
    r->xSink = NULL; r->pArg = NULL;
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    txnFree(r);
    pthread_mutex_unlock(&r->mu);
  }
  pthread_mutex_unlock(&gMutex);
}

int hpplite_walread_poll(const char *zPath, unsigned mxFrame) {
  Reader *r;
  int rc;
  if (!zPath) return 0;
  pthread_mutex_lock(&gMutex);
  r = find(zPath);
  if (r && !r->xSink) r = NULL;
  if (r) pthread_mutex_lock(&r->mu);
  pthread_mutex_unlock(&gMutex);
  if (!r) return 0;
  rc = pollLocked(r, mxFrame);
  pthread_mutex_unlock(&r->mu);
  return rc;
}

unsigned hpplite_walread_cursor(const char *zPath) {
  Reader *r;
  unsigned n = 0;
  pthread_mutex_lock(&gMutex);
  r = zPath ? find(zPath) : NULL;
  if (r) { pthread_mutex_lock(&r->mu); n = r->next; pthread_mutex_unlock(&r->mu); }
  pthread_mutex_unlock(&gMutex);
  return n;
}
