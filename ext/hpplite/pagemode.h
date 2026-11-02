/*
** Page mode (#146): what a sequencer keeps between the VFS's commits and the
** batches it posts. The sink queues each commit's pages; a build takes queued
** commits into a page batch (the dirty set as diffs against the images the
** last finalized batch left, the root after); a finalize or a failure settles
** it. One thread builds and settles; the sink is the application's thread.
*/
#ifndef HPPLITE_PAGEMODE_H
#define HPPLITE_PAGEMODE_H
#include <stdint.h>
#include <stddef.h>

typedef struct HpplitePageMode HpplitePageMode;
/* Start on a database file: its pages are read once (the root, the images
** diffs are taken against) and its commits are watched from here on. The
** caller has checkpointed it, so the file holds everything. */
HpplitePageMode *hpplite_pagemode_start(const char *zPath, int bWatch);   /* bWatch 0: a replica, applying what it is sent */
void hpplite_pagemode_stop(HpplitePageMode *pm);
/* The file as it is, whole, as the next batch (every page full): a new rollup's
** first batch, or a sequencer back on a file the chain does not have all of */
int hpplite_pagemode_snapshot(HpplitePageMode *pm);
/* commits waiting to be posted (queued, or in the batch being settled) */
unsigned hpplite_pagemode_pending(HpplitePageMode *pm);
size_t hpplite_pagemode_pending_bytes(HpplitePageMode *pm);
/* Take queued commits into a batch of at most maxBytes of page records (at
** least one commit). *ppBytes (sqlite3_free) are the batch's bytes, root the
** root after it; *pMore says more commits wait. SQLITE_DONE: nothing queued. */
int hpplite_pagemode_build(HpplitePageMode *pm, size_t maxBytes, unsigned char **ppBytes, size_t *pLen, unsigned char root[32], int *pMore);
/* the same with exactly nCommits taken (adopting a batch found on the chain) */
int hpplite_pagemode_build_n(HpplitePageMode *pm, unsigned nCommits, unsigned char **ppBytes, size_t *pLen, unsigned char root[32], int *pMore);
/* a replica: the pages a batch applied, into the root */
int hpplite_pagemode_applied(HpplitePageMode *pm, unsigned pgno, const unsigned char *page);
int hpplite_pagemode_applied_size(HpplitePageMode *pm, unsigned nPages);
/* the built batch is finalized: its images are now the base for the next */
void hpplite_pagemode_finalized(HpplitePageMode *pm);
/* the built batch was not finalized: the root goes back, its commits return to the queue */
void hpplite_pagemode_failed(HpplitePageMode *pm);
/* the root after the last finalized batch */
void hpplite_pagemode_root(HpplitePageMode *pm, unsigned char out[32]);
int hpplite_pagemode_page_size(HpplitePageMode *pm);

#endif
