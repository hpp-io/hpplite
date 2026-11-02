/*
** HPPLite - page capture by reading the WAL file (#146, #155).
**
** A watched database's commits, as the pages its WAL frames carry, handed
** to a sink: the reader keeps a cursor into <db>-wal (the WAL's generation,
** its salts, and the next frame to read) and, when polled, reads the frames
** committed since, validates them (salts, the cumulative checksum) and
** hands each commit's pages over. Polled from a connection's wal hook on
** the committing thread with the frame count SQLite gives it, and from the
** sequencer's timer with none, for frames another process wrote.
**
** A WAL restart (new salts after a checkpoint) starts the cursor over. The
** sequencer keeps a read transaction pinned at or before the cursor
** (node.c), so SQLite never backfills past what was read and no restart
** can drop a frame the reader has not seen.
*/
#ifndef HPPLITE_WALREAD_H
#define HPPLITE_WALREAD_H

typedef struct HppliteWalCommit HppliteWalCommit;
struct HppliteWalCommit {
  const char *zPath;                   /* the database (canonical path) */
  int pageSize;
  unsigned nPages;                     /* pages the transaction wrote, each once (its last image) */
  const unsigned *aPgno;
  const unsigned char *const *aImg;    /* pageSize bytes each, valid during the call */
  unsigned nTruncate;                  /* the database's size in pages after the commit */
  unsigned salt1, salt2;               /* the WAL header's salts: its generation */
  unsigned frame;                      /* the commit frame's index in that WAL (1-based) */
};
/* Called with the reader's lock held: copy what is needed and return; never
** touch the database. */
typedef void (*HppliteWalSink)(void *pArg, const HppliteWalCommit *pCommit);

/* Watch a database file (its canonical path): commits found in its WAL from
** frame startFrame+1 on reach the sink (startFrame: frames the file already
** holds, a checkpoint's count). One sink per file. */
int hpplite_walread_watch(const char *zPath, HppliteWalSink xSink, void *pArg, unsigned startFrame);
void hpplite_walread_unwatch(const char *zPath);

/* Read what was committed since the cursor: up to frame mxFrame (0: to the
** end of the file, stopping before a transaction still in flight). The
** number of commits delivered, or a negative rc. No watch: 0. */
int hpplite_walread_poll(const char *zPath, unsigned mxFrame);

/* The reader's cursor: the frame it will read next, 0 with no watch */
unsigned hpplite_walread_cursor(const char *zPath);

#endif
