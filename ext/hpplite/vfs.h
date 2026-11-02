/*
** HPPLite VFS, the capture side (#146): a watched database's commits, as
** the pages its WAL frames carry, handed to a sink on the committing thread.
** The clock half of the same VFS is in clock.h. See clock.c.
*/
#ifndef HPPLITE_VFS_H
#define HPPLITE_VFS_H

typedef struct HppliteVfsCommit HppliteVfsCommit;
struct HppliteVfsCommit {
  const char *zPath;                   /* the database (canonical path) */
  int pageSize;
  unsigned nPages;                     /* pages the transaction wrote, each once (its last image) */
  const unsigned *aPgno;
  const unsigned char *const *aImg;    /* pageSize bytes each, valid during the call */
  unsigned nTruncate;                  /* the database's size in pages after the commit */
  unsigned salt1, salt2;               /* the WAL header's salts: its generation */
  unsigned frame;                      /* the commit frame's index in that WAL (0-based) */
};
/* Called on the committing thread, inside the WAL write that completes the
** transaction: copy what is needed and return; never touch the database. */
typedef void (*HppliteVfsSink)(void *pArg, const HppliteVfsCommit *pCommit);

/* Watch a database file (its path as sqlite3_db_filename gives it): every
** commit a connection through this VFS makes to it reaches the sink. A WAL
** already open for the file is picked up. One sink per file. */
int hpplite_vfs_watch(const char *zPath, HppliteVfsSink xSink, void *pArg);
void hpplite_vfs_unwatch(const char *zPath);

#endif
