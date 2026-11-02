/*
** HPPLite clock: a pass-through VFS that can pin SQLite's idea of "now" (#56)
**
** SQLite takes the current time from the connection's VFS
** (xCurrentTimeInt64) - CURRENT_TIMESTAMP, datetime('now'), unixepoch(),
** DEFAULT CURRENT_TIMESTAMP all go through it. This VFS forwards every call
** to the VFS that was the default when it was registered, except the clock:
** while a time is pinned on the calling thread, that time is returned.
**
** The writer pins the real time when it captures a write and records it
** with the statement if the statement consulted the clock; replay pins the
** recorded time. Both sides then compute the same values.
**
** Registered once per process as the default VFS (static library: at load;
** loadable module: when it is loaded), so connections opened afterwards use
** it. A connection that names another VFS is not covered: HPPLite keeps
** refusing clock SQL there.
*/
#include "hpplite_sqlite.h"
#include "clock.h"
#include "vfs.h"
#include <pthread.h>
#include <string.h>
#include <stdlib.h>

/* ---- page capture (#146) ----
** Every file this VFS opens is wrapped; a WAL file of a watched database has
** its writes read: the 32-byte header (page size, salts: a new generation,
** frames start over), then frames of a 24-byte header (page number, and the
** database size when the frame commits) and a page. A transaction's pages
** are collected by page number (SQLite rewrites a page's frame when the same
** page is written twice in one transaction) and handed to the sink at the
** commit frame. */
typedef struct Watch Watch;
struct Watch {
  char *zPath;
  HppliteVfsSink xSink;
  void *pArg;
  int pageSize;
  unsigned salt1, salt2;
  unsigned *aFramePgno; unsigned nFrames;            /* frame index -> page number, bit 31: commits */
  unsigned pendingTruncate;                          /* the commit frame's database size, when its header came alone */
  unsigned char **aImg; unsigned *aPgno; unsigned nTxn, nTxnAlloc;
  Watch *pNext;
};
typedef struct VfsFile VfsFile;
struct VfsFile {
  sqlite3_file base;
  sqlite3_file *pReal;                 /* the parent VFS's file, inline after this struct */
  char *zPath;                         /* a main database or a WAL: the database's path */
  int isWal;
  VfsFile *pNextWal;                   /* open WAL files, for a watch that comes later */
};
static pthread_mutex_t gWatchMutex = PTHREAD_MUTEX_INITIALIZER;
static Watch *gWatches;
static VfsFile *gWals;

static unsigned be32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }

static Watch *watchFind(const char *zPath) {
  Watch *w;
  for (w = gWatches; w; w = w->pNext) if (strcmp(w->zPath, zPath) == 0) return w;
  return NULL;
}

static void txnAdd(Watch *w, unsigned pg, const unsigned char *img) {
  unsigned i;
  for (i = 0; i < w->nTxn; i++) if (w->aPgno[i] == pg) { memcpy(w->aImg[i], img, (size_t)w->pageSize); return; }
  if (w->nTxn == w->nTxnAlloc) {
    unsigned n = w->nTxnAlloc ? w->nTxnAlloc * 2 : 32;
    w->aImg = sqlite3_realloc64(w->aImg, (sqlite3_uint64)n * sizeof(*w->aImg));
    w->aPgno = sqlite3_realloc64(w->aPgno, (sqlite3_uint64)n * sizeof(*w->aPgno));
    w->nTxnAlloc = n;
  }
  w->aImg[w->nTxn] = sqlite3_malloc64((sqlite3_uint64)w->pageSize);
  memcpy(w->aImg[w->nTxn], img, (size_t)w->pageSize);
  w->aPgno[w->nTxn] = pg;
  w->nTxn++;
}

static void txnCommit(Watch *w, unsigned frame, unsigned nTruncate) {
  HppliteVfsCommit c;
  unsigned i;
  c.zPath = w->zPath; c.pageSize = w->pageSize; c.nPages = w->nTxn; c.aPgno = w->aPgno;
  c.aImg = (const unsigned char *const *)w->aImg; c.nTruncate = nTruncate;
  c.salt1 = w->salt1; c.salt2 = w->salt2; c.frame = frame;
  if (w->xSink && w->nTxn) w->xSink(w->pArg, &c);
  for (i = 0; i < w->nTxn; i++) sqlite3_free(w->aImg[i]);
  w->nTxn = 0;
}

static void walWrite(Watch *w, const unsigned char *p, int amt, sqlite3_int64 off) {
  if (off == 0 && amt >= 32) {
    w->pageSize = (int)be32(p + 8);
    w->salt1 = be32(p + 16); w->salt2 = be32(p + 20);
    w->nFrames = 0; w->nTxn = 0;                     /* a new WAL, or a restart: frames start over */
    if (amt == 32) return;
    p += 32; amt -= 32; off = 32;
  }
  if (w->pageSize <= 0) return;
  while (amt > 0) {
    sqlite3_int64 frameSz = 24 + w->pageSize, rel = off - 32;
    unsigned idx = (unsigned)(rel / frameSz);
    int within = (int)(rel % frameSz);
    if (within == 0 && amt >= 24) {
      unsigned pg = be32(p), nTrunc = be32(p + 4);
      if (idx >= w->nFrames) {
        w->aFramePgno = sqlite3_realloc64(w->aFramePgno, (sqlite3_uint64)(idx + 1) * sizeof(unsigned));
        w->nFrames = idx + 1;
      }
      if (amt >= 24 + w->pageSize) {                 /* header and page in one write */
        w->aFramePgno[idx] = pg;
        txnAdd(w, pg, p + 24);
        if (nTrunc) txnCommit(w, idx, nTrunc);
        p += 24 + w->pageSize; amt -= 24 + w->pageSize; off += 24 + w->pageSize;
        continue;
      }
      w->aFramePgno[idx] = pg | (nTrunc ? 0x80000000u : 0);   /* the page follows in the next write */
      if (nTrunc) w->pendingTruncate = nTrunc;
      p += 24; amt -= 24; off += 24;
      continue;
    }
    if (within == 24 && amt >= w->pageSize && idx < w->nFrames) {
      unsigned pg = w->aFramePgno[idx] & 0x7fffffffu;
      txnAdd(w, pg, p);
      if (w->aFramePgno[idx] & 0x80000000u) txnCommit(w, idx, w->pendingTruncate);
      p += w->pageSize; amt -= w->pageSize; off += w->pageSize;
      continue;
    }
    break;                                           /* a partial write this does not model */
  }
}

/* A WAL already written when the watch begins, or opened by a later
** connection: its header says the page size and the salts; frames written
** before are not ours to see, the ones after are */
static void watchLearnHeader(Watch *w, sqlite3_file *pReal) {
  unsigned char hdr[32];
  if (w->pageSize > 0 || !pReal || !pReal->pMethods) return;
  if (pReal->pMethods->xRead(pReal, hdr, 32, 0) != SQLITE_OK) return;
  if (be32(hdr) != 0x377f0682u && be32(hdr) != 0x377f0683u) return;      /* not a WAL header (an empty file reads as short) */
  w->pageSize = (int)be32(hdr + 8); w->salt1 = be32(hdr + 16); w->salt2 = be32(hdr + 20);
}

int hpplite_vfs_watch(const char *zPath, HppliteVfsSink xSink, void *pArg) {
  Watch *w;
  VfsFile *f;
  if (!zPath) return SQLITE_MISUSE;
  pthread_mutex_lock(&gWatchMutex);
  w = watchFind(zPath);
  if (!w) {
    w = sqlite3_malloc64(sizeof(*w));
    if (!w) { pthread_mutex_unlock(&gWatchMutex); return SQLITE_NOMEM; }
    memset(w, 0, sizeof(*w));
    w->zPath = sqlite3_mprintf("%s", zPath);
    w->pNext = gWatches; gWatches = w;
  }
  w->xSink = xSink; w->pArg = pArg;
  for (f = gWals; f; f = f->pNextWal) if (f->zPath && strcmp(f->zPath, zPath) == 0) watchLearnHeader(w, f->pReal);
  pthread_mutex_unlock(&gWatchMutex);
  return SQLITE_OK;
}

void hpplite_vfs_unwatch(const char *zPath) {
  Watch *w;
  pthread_mutex_lock(&gWatchMutex);
  w = zPath ? watchFind(zPath) : NULL;
  if (w) { w->xSink = NULL; w->pArg = NULL; }
  pthread_mutex_unlock(&gWatchMutex);
}

static sqlite3_vfs gClock;               /* the shim */
static sqlite3_vfs *gOrig;               /* the VFS it forwards to */
static pthread_once_t gOnce = PTHREAD_ONCE_INIT;
static int gRegistered;

static _Thread_local sqlite3_int64 tlPinned;    /* Julian day ms; 0 = not pinned */
static _Thread_local unsigned tlConsulted;      /* clock reads while pinned */

static int clockCurrentTimeInt64(sqlite3_vfs *pVfs, sqlite3_int64 *piNow) {
  (void)pVfs;
  if (tlPinned) {
    tlConsulted++;
    *piNow = tlPinned;
    return SQLITE_OK;
  }
  if (gOrig->iVersion >= 2 && gOrig->xCurrentTimeInt64) return gOrig->xCurrentTimeInt64(gOrig, piNow);
  {
    double r = 0;
    int rc = gOrig->xCurrentTime(gOrig, &r);
    *piNow = (sqlite3_int64)(r * 86400000.0);
    return rc;
  }
}
static int clockCurrentTime(sqlite3_vfs *pVfs, double *prNow) {
  sqlite3_int64 i = 0;
  int rc = clockCurrentTimeInt64(pVfs, &i);
  *prNow = i / 86400000.0;
  return rc;
}

/* ---- the file wrapper: every call forwarded, a watched WAL's writes read ---- */
#define REAL(f) (((VfsFile *)(f))->pReal)
static int fClose(sqlite3_file *f) {
  VfsFile *p = (VfsFile *)f;
  int rc = REAL(f)->pMethods->xClose(REAL(f));
  if (p->isWal) {
    VfsFile **pp;
    pthread_mutex_lock(&gWatchMutex);
    for (pp = &gWals; *pp; pp = &(*pp)->pNextWal) if (*pp == p) { *pp = p->pNextWal; break; }
    pthread_mutex_unlock(&gWatchMutex);
  }
  sqlite3_free(p->zPath); p->zPath = NULL;
  return rc;
}
static int fRead(sqlite3_file *f, void *b, int n, sqlite3_int64 o) { return REAL(f)->pMethods->xRead(REAL(f), b, n, o); }
static int fWrite(sqlite3_file *f, const void *b, int n, sqlite3_int64 o) {
  VfsFile *p = (VfsFile *)f;
  int rc = REAL(f)->pMethods->xWrite(REAL(f), b, n, o);
  if (rc == SQLITE_OK && p->isWal && p->zPath) {
    Watch *w;
    pthread_mutex_lock(&gWatchMutex);
    w = watchFind(p->zPath);
    pthread_mutex_unlock(&gWatchMutex);
    /* one writer per WAL at a time (SQLite's write lock): the watch's state is this thread's */
    if (w) walWrite(w, (const unsigned char *)b, n, o);
  }
  return rc;
}
static int fTruncate(sqlite3_file *f, sqlite3_int64 n) { return REAL(f)->pMethods->xTruncate(REAL(f), n); }
static int fSync(sqlite3_file *f, int fl) { return REAL(f)->pMethods->xSync(REAL(f), fl); }
static int fFileSize(sqlite3_file *f, sqlite3_int64 *n) { return REAL(f)->pMethods->xFileSize(REAL(f), n); }
static int fLock(sqlite3_file *f, int l) { return REAL(f)->pMethods->xLock(REAL(f), l); }
static int fUnlock(sqlite3_file *f, int l) { return REAL(f)->pMethods->xUnlock(REAL(f), l); }
static int fCheckReservedLock(sqlite3_file *f, int *r) { return REAL(f)->pMethods->xCheckReservedLock(REAL(f), r); }
static int fFileControl(sqlite3_file *f, int op, void *a) {
  int rc = REAL(f)->pMethods->xFileControl(REAL(f), op, a);
  if (op == SQLITE_FCNTL_VFS_POINTER && rc == SQLITE_OK) *(sqlite3_vfs **)a = &gClock;   /* ours, to whoever asks (hpplite_clock_on) */
  return rc;
}
static int fSectorSize(sqlite3_file *f) { return REAL(f)->pMethods->xSectorSize(REAL(f)); }
static int fDeviceCharacteristics(sqlite3_file *f) { return REAL(f)->pMethods->xDeviceCharacteristics(REAL(f)); }
static int fShmMap(sqlite3_file *f, int i, int sz, int ext, void volatile **pp) { return REAL(f)->pMethods->xShmMap(REAL(f), i, sz, ext, pp); }
static int fShmLock(sqlite3_file *f, int o, int n, int fl) { return REAL(f)->pMethods->xShmLock(REAL(f), o, n, fl); }
static void fShmBarrier(sqlite3_file *f) { REAL(f)->pMethods->xShmBarrier(REAL(f)); }
static int fShmUnmap(sqlite3_file *f, int del) { return REAL(f)->pMethods->xShmUnmap(REAL(f), del); }
static int fFetch(sqlite3_file *f, sqlite3_int64 o, int n, void **pp) {
  if (REAL(f)->pMethods->iVersion >= 3 && REAL(f)->pMethods->xFetch) return REAL(f)->pMethods->xFetch(REAL(f), o, n, pp);
  *pp = NULL; return SQLITE_OK;
}
static int fUnfetch(sqlite3_file *f, sqlite3_int64 o, void *p) {
  if (REAL(f)->pMethods->iVersion >= 3 && REAL(f)->pMethods->xUnfetch) return REAL(f)->pMethods->xUnfetch(REAL(f), o, p);
  return SQLITE_OK;
}
static const sqlite3_io_methods gIo = {
  3, fClose, fRead, fWrite, fTruncate, fSync, fFileSize, fLock, fUnlock, fCheckReservedLock,
  fFileControl, fSectorSize, fDeviceCharacteristics, fShmMap, fShmLock, fShmBarrier, fShmUnmap, fFetch, fUnfetch
};

static int clockOpen(sqlite3_vfs *pVfs, const char *z, sqlite3_file *f, int fl, int *pOut) {
  VfsFile *p = (VfsFile *)f;
  int rc;
  (void)pVfs;
  p->pReal = (sqlite3_file *)&p[1];
  p->zPath = NULL; p->isWal = 0; p->pNextWal = NULL;
  rc = gOrig->xOpen(gOrig, z, p->pReal, fl, pOut);
  if (rc != SQLITE_OK) { f->pMethods = NULL; return rc; }
  if (!p->pReal->pMethods) { f->pMethods = NULL; return SQLITE_OK; }   /* a file the parent did not really open */
  if (z && (fl & SQLITE_OPEN_WAL)) {
    size_t n = strlen(z);
    if (n > 4 && strcmp(z + n - 4, "-wal") == 0) {
      p->zPath = sqlite3_mprintf("%.*s", (int)(n - 4), z);
      p->isWal = 1;
      pthread_mutex_lock(&gWatchMutex);
      p->pNextWal = gWals; gWals = p;
      { Watch *w = watchFind(p->zPath); if (w) watchLearnHeader(w, p->pReal); }
      pthread_mutex_unlock(&gWatchMutex);
    }
  }
  f->pMethods = &gIo;
  return SQLITE_OK;
}
static int clockDelete(sqlite3_vfs *p, const char *z, int sync) { (void)p; return gOrig->xDelete(gOrig, z, sync); }
static int clockAccess(sqlite3_vfs *p, const char *z, int fl, int *pRes) { (void)p; return gOrig->xAccess(gOrig, z, fl, pRes); }
static int clockFullPathname(sqlite3_vfs *p, const char *z, int n, char *out) {
  (void)p; return gOrig->xFullPathname(gOrig, z, n, out);
}
static void *clockDlOpen(sqlite3_vfs *p, const char *z) { (void)p; return gOrig->xDlOpen(gOrig, z); }
static void clockDlError(sqlite3_vfs *p, int n, char *z) { (void)p; gOrig->xDlError(gOrig, n, z); }
static void (*clockDlSym(sqlite3_vfs *p, void *h, const char *z))(void) { (void)p; return gOrig->xDlSym(gOrig, h, z); }
static void clockDlClose(sqlite3_vfs *p, void *h) { (void)p; gOrig->xDlClose(gOrig, h); }
static int clockRandomness(sqlite3_vfs *p, int n, char *z) { (void)p; return gOrig->xRandomness(gOrig, n, z); }
static int clockSleep(sqlite3_vfs *p, int us) { (void)p; return gOrig->xSleep(gOrig, us); }
static int clockGetLastError(sqlite3_vfs *p, int n, char *z) {
  (void)p; return gOrig->xGetLastError ? gOrig->xGetLastError(gOrig, n, z) : 0;
}
static int clockSetSystemCall(sqlite3_vfs *p, const char *z, sqlite3_syscall_ptr f) {
  (void)p; return gOrig->xSetSystemCall(gOrig, z, f);
}
static sqlite3_syscall_ptr clockGetSystemCall(sqlite3_vfs *p, const char *z) { (void)p; return gOrig->xGetSystemCall(gOrig, z); }
static const char *clockNextSystemCall(sqlite3_vfs *p, const char *z) { (void)p; return gOrig->xNextSystemCall(gOrig, z); }

static void clockRegisterOnce(void) {
  gOrig = sqlite3_vfs_find(NULL);
  if (!gOrig || gOrig == &gClock) return;
  memset(&gClock, 0, sizeof(gClock));
  gClock.iVersion = gOrig->iVersion < 3 ? gOrig->iVersion : 3;
  if (gClock.iVersion < 2) gClock.iVersion = 2;      /* our xCurrentTimeInt64 is always there */
  gClock.szOsFile = (int)sizeof(VfsFile) + gOrig->szOsFile;   /* our wrapper, the parent's file after it */
  gClock.mxPathname = gOrig->mxPathname;
  gClock.zName = "hpplite";
  gClock.xOpen = clockOpen;
  gClock.xDelete = clockDelete;
  gClock.xAccess = clockAccess;
  gClock.xFullPathname = clockFullPathname;
  gClock.xDlOpen = gOrig->xDlOpen ? clockDlOpen : NULL;
  gClock.xDlError = gOrig->xDlError ? clockDlError : NULL;
  gClock.xDlSym = gOrig->xDlSym ? clockDlSym : NULL;
  gClock.xDlClose = gOrig->xDlClose ? clockDlClose : NULL;
  gClock.xRandomness = clockRandomness;
  gClock.xSleep = clockSleep;
  gClock.xCurrentTime = clockCurrentTime;
  gClock.xGetLastError = clockGetLastError;
  gClock.xCurrentTimeInt64 = clockCurrentTimeInt64;
  if (gClock.iVersion >= 3 && gOrig->iVersion >= 3) {
    gClock.xSetSystemCall = gOrig->xSetSystemCall ? clockSetSystemCall : NULL;
    gClock.xGetSystemCall = gOrig->xGetSystemCall ? clockGetSystemCall : NULL;
    gClock.xNextSystemCall = gOrig->xNextSystemCall ? clockNextSystemCall : NULL;
  } else {
    gClock.iVersion = 2;
  }
  if (sqlite3_vfs_register(&gClock, 1) == SQLITE_OK) gRegistered = 1;
}

int hpplite_clock_register(void) {
  pthread_once(&gOnce, clockRegisterOnce);
  return gRegistered ? SQLITE_OK : SQLITE_ERROR;
}

int hpplite_clock_on(sqlite3 *db) {
  sqlite3_vfs *p = NULL;
  if (!gRegistered || !db) return 0;
  if (sqlite3_file_control(db, "main", SQLITE_FCNTL_VFS_POINTER, &p) != SQLITE_OK) return 0;
  return p == &gClock;
}

sqlite3_int64 hpplite_clock_read(void) {
  sqlite3_int64 t = 0;
  sqlite3_int64 saved = tlPinned;
  if (!gOrig) return 0;
  tlPinned = 0;
  clockCurrentTimeInt64(&gClock, &t);
  tlPinned = saved;
  return t;
}

void hpplite_clock_pin(sqlite3_int64 t) {
  tlPinned = t;
  tlConsulted = 0;
}

sqlite3_int64 hpplite_clock_pinned(void) {
  return tlPinned;
}

unsigned hpplite_clock_consulted(void) {
  return tlConsulted;
}

void hpplite_clock_restore(sqlite3_int64 t, unsigned consulted) {
  tlPinned = t;
  tlConsulted = consulted;
}
