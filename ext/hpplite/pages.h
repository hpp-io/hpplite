/*
** Page-based capture (#146): the root over a database's pages, and the batch
** that ships the pages a run of commits changed.
**
** The root is a Merkle tree over SHA-256("HPP4" || pgno || page) for pages
** 1..n, a dirty page costing one path, kept in memory (32 bytes a node, about
** 2n nodes) and computed from the file itself when a node opens. A batch is
** LTX-shaped: a header, one record per dirty page (its image, or its XOR
** diff against the image the previous batch left), a trailer with the root
** after the batch and a CRC64 rolling checksum of the file (LTX's scheme:
** the XOR of each page's CRC64, O(1) per dirty page, the quick self-check).
*/
#ifndef HPPLITE_PAGES_H
#define HPPLITE_PAGES_H
#include <stdint.h>
#include <stddef.h>

/* ---- the root ---- */
typedef struct HpplitePageRoot HpplitePageRoot;
HpplitePageRoot *hpplite_pageroot_new(int pageSize);
void hpplite_pageroot_free(HpplitePageRoot *pr);
/* page pgno (1-based) is now this image */
int hpplite_pageroot_set(HpplitePageRoot *pr, unsigned pgno, const unsigned char *page);
/* the file is nPages long: pages beyond are gone */
int hpplite_pageroot_truncate(HpplitePageRoot *pr, unsigned nPages);
unsigned hpplite_pageroot_pages(const HpplitePageRoot *pr);
/* SHA-256 over (page size, page count, the Merkle root): what a batch's trailer carries */
void hpplite_pageroot_get(const HpplitePageRoot *pr, unsigned char out[32]);
/* the rolling CRC64 (XOR of the pages' CRC64s), kept beside the root */
uint64_t hpplite_pageroot_crc(const HpplitePageRoot *pr);
/* from a database file as it is (a node opening, a verifier): every page read once */
HpplitePageRoot *hpplite_pageroot_from_file(const char *zPath, int *pPageSize);
/* A database file read through SQLite's own VFS, never a plain open(): the
** close of any descriptor on a file drops every POSIX lock the process holds
** on it, its connections' SHARED lock among them, and an application's last
** close then checkpoints and deletes the WAL under them (somaro#116). The
** VFS keeps one lock set per file and holds a closed descriptor open while a
** lock is. NULL: no such file. A read past the end gives fewer than n. */
typedef struct HppliteDbFile HppliteDbFile;
HppliteDbFile *hpplite_dbfile_open(const char *zPath);
size_t hpplite_dbfile_read(HppliteDbFile *f, void *buf, size_t n, int64_t off);
void hpplite_dbfile_close(HppliteDbFile *f);
uint64_t hpplite_crc64(uint64_t crc, const unsigned char *p, size_t n);

/* ---- the batch ---- */
#define HPPLITE_PAGE_FULL 0
#define HPPLITE_PAGE_DIFF 1                  /* body = image XOR the image the previous batch left */
typedef struct HpplitePageRec { unsigned pgno; unsigned flags; const unsigned char *body; } HpplitePageRec;
typedef struct HpplitePageBatch {
  unsigned pageSize;
  unsigned commitSize;                       /* the file's size in pages after the batch */
  uint64_t firstCommit, lastCommit;          /* the commits it covers (the sequencer's count) */
  unsigned salt1, salt2;                     /* the WAL generation the last commit was in */
  uint64_t timestampMs;
  unsigned nPages;
  HpplitePageRec *aPage;                     /* sorted by pgno */
  unsigned char root[32];                    /* hpplite_pageroot_get after the batch */
  uint64_t crc;                              /* hpplite_pageroot_crc after the batch */
  const char *zName; unsigned nName;         /* the database's name (dbname=, #160), on batch 1; not NUL-terminated when decoded */
  unsigned piece;                            /* 0: a whole batch; k: the k-th piece of one commit cut over batches (#152) */
  unsigned bMore;                            /* a piece with pieces after it: a replica keeps its lock until the last */
} HpplitePageBatch;
/* is this a page batch (before any parse)? */
int hpplite_pagebatch_is(const unsigned char *p, size_t n);
/* A page batch's writer time (ms) and whether its commit continues in the next batch, from its header (#205): 0, or -1 */
int hpplite_pagebatch_meta(const unsigned char *p, size_t n, uint64_t *pTimestampMs, int *pMore);
/* the bytes of a batch; sqlite3_free */
unsigned char *hpplite_pagebatch_encode(const HpplitePageBatch *b, size_t *pLen);
/* a batch from its bytes: the records point into p, which must outlive it; sqlite3_free the batch */
HpplitePageBatch *hpplite_pagebatch_decode(const unsigned char *p, size_t n);
void hpplite_pagebatch_free(HpplitePageBatch *b);

#endif
