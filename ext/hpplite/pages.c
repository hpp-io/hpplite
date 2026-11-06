/* Page-based capture (#146): the page root and the page batch. See pages.h. */
#include "hpplite_sqlite.h"
#include "pages.h"
#include "sha256.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------- CRC64 */
uint64_t hpplite_crc64(uint64_t crc, const unsigned char *p, size_t n) {
  static uint64_t tab[256];
  static int init;
  size_t i;
  if (!init) {
    int k;
    for (i = 0; i < 256; i++) { uint64_t c = i; for (k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xC96C5795D7870F42ull : c >> 1; tab[i] = c; }
    init = 1;
  }
  crc = ~crc;
  for (i = 0; i < n; i++) crc = tab[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

/* ---------------------------------------------------------- the root */
struct HpplitePageRoot {
  int pageSize;
  unsigned nPages;                 /* the file's length in pages */
  unsigned cap;                    /* leaves allocated, a power of two >= nPages */
  unsigned char *aNode;            /* 2*cap nodes of 32 bytes: node i's children are 2i and 2i+1, leaves from cap */
  uint64_t *aCrc;                  /* per page, for the rolling checksum */
  uint64_t crc;
};
#define NODE(pr, i) ((pr)->aNode + (size_t)(i) * 32)

/* Page 1's file format bytes (18-19: 1 in rollback-journal mode, 2 in WAL) and
** its change counters (24-27, 92-95: moved by a rollback-mode replica so its
** readers notice) are not content: a WAL-mode sequencer and a rollback-mode
** replica hash to one root. */
static void leafHash(unsigned pgno, const unsigned char *page, int pageSize, unsigned char out[32]) {
  unsigned char tag[8] = { 'H', 'P', 'P', '4', (unsigned char)(pgno >> 24), (unsigned char)(pgno >> 16), (unsigned char)(pgno >> 8), (unsigned char)pgno };
  if (pgno == 1 && pageSize >= 100) {
    unsigned char *masked = sqlite3_malloc64((sqlite3_uint64)pageSize);
    if (masked) {
      memcpy(masked, page, (size_t)pageSize);
      masked[18] = masked[19] = 0; memset(masked + 24, 0, 4); memset(masked + 92, 0, 4);
      hpplite_sha256_prefixed(tag, 8, masked, (size_t)pageSize, out);
      sqlite3_free(masked);
      return;
    }
  }
  hpplite_sha256_prefixed(tag, 8, page, (size_t)pageSize, out);
}
static void innerHash(const unsigned char *l, const unsigned char *r, unsigned char out[32]) {
  hpplite_sha256_prefixed(l, 32, r, 32, out);
}
static void pathUp(HpplitePageRoot *pr, unsigned leaf) {
  unsigned i = pr->cap + leaf;
  while (i > 1) { i >>= 1; innerHash(NODE(pr, 2 * i), NODE(pr, 2 * i + 1), NODE(pr, i)); }
}

HpplitePageRoot *hpplite_pageroot_new(int pageSize) {
  HpplitePageRoot *pr = sqlite3_malloc64(sizeof(*pr));
  if (!pr) return NULL;
  memset(pr, 0, sizeof(*pr));
  pr->pageSize = pageSize;
  return pr;
}
void hpplite_pageroot_free(HpplitePageRoot *pr) {
  if (!pr) return;
  sqlite3_free(pr->aNode); sqlite3_free(pr->aCrc); sqlite3_free(pr);
}

/* room for n leaves: a bigger tree is rebuilt from the leaves (rare: the file doubled) */
static int grow(HpplitePageRoot *pr, unsigned n) {
  unsigned cap = pr->cap ? pr->cap : 16, i;
  unsigned char *aNew; uint64_t *aCrc;
  while (cap < n) cap *= 2;
  if (cap == pr->cap) return SQLITE_OK;
  aNew = sqlite3_malloc64((sqlite3_uint64)cap * 2 * 32);
  aCrc = NULL;
  if (!aNew) return SQLITE_NOMEM;
  memset(aNew, 0, (size_t)cap * 2 * 32);
  if (pr->aNode) memcpy(aNew + (size_t)cap * 32, pr->aNode + (size_t)pr->cap * 32, (size_t)pr->cap * 32);
  sqlite3_free(pr->aNode);
  pr->aNode = aNew; pr->aCrc = aCrc; pr->cap = cap;
  for (i = cap - 1; i >= 1; i--) innerHash(NODE(pr, 2 * i), NODE(pr, 2 * i + 1), NODE(pr, i));
  return SQLITE_OK;
}

int hpplite_pageroot_set(HpplitePageRoot *pr, unsigned pgno, const unsigned char *page) {
  if (!pr || pgno == 0) return SQLITE_MISUSE;
  if (pgno > pr->cap && grow(pr, pgno) != SQLITE_OK) return SQLITE_NOMEM;
  leafHash(pgno, page, pr->pageSize, NODE(pr, pr->cap + pgno - 1));
  pathUp(pr, pgno - 1);
  if (pgno > pr->nPages) pr->nPages = pgno;
  return SQLITE_OK;
}

int hpplite_pageroot_truncate(HpplitePageRoot *pr, unsigned nPages) {
  unsigned i;
  if (!pr) return SQLITE_MISUSE;
  if (nPages > pr->cap && grow(pr, nPages) != SQLITE_OK) return SQLITE_NOMEM;
  for (i = nPages; i < pr->nPages; i++) {           /* pages beyond the end: absent */
    memset(NODE(pr, pr->cap + i), 0, 32);
    pathUp(pr, i);
  }
  pr->nPages = nPages;
  return SQLITE_OK;
}
unsigned hpplite_pageroot_pages(const HpplitePageRoot *pr) { return pr ? pr->nPages : 0; }
uint64_t hpplite_pageroot_crc(const HpplitePageRoot *pr) { (void)pr; return 0; }   /* no rolling checksum: the root is the integrity (#146) */

void hpplite_pageroot_get(const HpplitePageRoot *pr, unsigned char out[32]) {
  SHA256_CTX c;
  unsigned char hdr[12];
  unsigned ps = pr ? (unsigned)pr->pageSize : 0, n = pr ? pr->nPages : 0;
  hdr[0] = 'H'; hdr[1] = 'P'; hdr[2] = 'R'; hdr[3] = '4';
  hdr[4] = (unsigned char)(ps >> 24); hdr[5] = (unsigned char)(ps >> 16); hdr[6] = (unsigned char)(ps >> 8); hdr[7] = (unsigned char)ps;
  hdr[8] = (unsigned char)(n >> 24); hdr[9] = (unsigned char)(n >> 16); hdr[10] = (unsigned char)(n >> 8); hdr[11] = (unsigned char)n;
  hpplite_sha256_init(&c);
  hpplite_sha256_update(&c, hdr, 12);
  if (pr && pr->aNode && pr->cap) {
    /* the Merkle root over the smallest power of two of leaves that holds the
    ** pages, whatever the tree's capacity: the leftmost subtree of that size
    ** (a tree that grew and was truncated gives the root a fresh one gives) */
    unsigned need = 1, idx;
    while (need < n) need *= 2;
    idx = pr->cap / need;                            /* both powers of two: the subtree's node index */
    hpplite_sha256_update(&c, NODE(pr, idx), 32);
  }
  hpplite_sha256_final(&c, out);
}

HpplitePageRoot *hpplite_pageroot_from_file(const char *zPath, int *pPageSize) {
  FILE *f = fopen(zPath, "rb");
  unsigned char hdr[100], *page;
  int pageSize;
  unsigned n, i;
  HpplitePageRoot *pr;
  if (!f) return NULL;
  if (fread(hdr, 1, 100, f) != 100) { fclose(f); return NULL; }
  pageSize = (hdr[16] << 8) | hdr[17];
  if (pageSize == 1) pageSize = 65536;
  n = ((unsigned)hdr[28] << 24) | ((unsigned)hdr[29] << 16) | ((unsigned)hdr[30] << 8) | hdr[31];   /* the header's page count */
  pr = hpplite_pageroot_new(pageSize);
  page = sqlite3_malloc64((sqlite3_uint64)pageSize);
  if (!pr || !page || grow(pr, n ? n : 1) != SQLITE_OK) { hpplite_pageroot_free(pr); sqlite3_free(page); fclose(f); return NULL; }
  fseek(f, 0, SEEK_SET);
  for (i = 1; i <= n; i++) {
    if (fread(page, 1, (size_t)pageSize, f) != (size_t)pageSize) break;
    hpplite_pageroot_set(pr, i, page);
  }
  fclose(f);
  sqlite3_free(page);
  if (pPageSize) *pPageSize = pageSize;
  return pr;
}

/* --------------------------------------------------------- the batch */
#define PB_MAGIC "HPPL"
#define PB_VERSION 2u
#define PB_HEADER_V1 56u                               /* magic 4, version 4, pageSize 4, commitSize 4, first 8, last 8, salt1 4, salt2 4, time 8, nPages 4, nName 4 (the name follows the trailer, #160) */
#define PB_HEADER 64u                                  /* v2: ...piece 4, flags 4 (bit 0: more pieces follow) (#152) */
#define PB_TRAILER 40u                                 /* root 32, crc 8 */

static void w32(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16); p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v; }
static void w64(unsigned char *p, uint64_t v) { w32(p, (unsigned)(v >> 32)); w32(p + 4, (unsigned)v); }
static unsigned r32(const unsigned char *p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static uint64_t r64(const unsigned char *p) { return ((uint64_t)r32(p) << 32) | r32(p + 4); }

int hpplite_pagebatch_is(const unsigned char *p, size_t n) {
  return p && n >= PB_HEADER_V1 && memcmp(p, PB_MAGIC, 4) == 0 && (r32(p + 4) == 1u || r32(p + 4) == PB_VERSION);
}

unsigned char *hpplite_pagebatch_encode(const HpplitePageBatch *b, size_t *pLen) {
  unsigned nName = b->zName ? b->nName : 0;
  size_t rec = 6 + (size_t)b->pageSize, len = PB_HEADER + rec * b->nPages + PB_TRAILER + nName, o = PB_HEADER;
  unsigned char *p = sqlite3_malloc64(len);
  unsigned i;
  if (!p) return NULL;
  memcpy(p, PB_MAGIC, 4); w32(p + 4, PB_VERSION); w32(p + 8, b->pageSize); w32(p + 12, b->commitSize);
  w64(p + 16, b->firstCommit); w64(p + 24, b->lastCommit); w32(p + 32, b->salt1); w32(p + 36, b->salt2);
  w64(p + 40, b->timestampMs); w32(p + 48, b->nPages); w32(p + 52, nName);
  w32(p + 56, b->piece); w32(p + 60, b->bMore ? 1u : 0u);
  for (i = 0; i < b->nPages; i++) {
    w32(p + o, b->aPage[i].pgno); p[o + 4] = (unsigned char)(b->aPage[i].flags >> 8); p[o + 5] = (unsigned char)b->aPage[i].flags;
    memcpy(p + o + 6, b->aPage[i].body, (size_t)b->pageSize);
    o += rec;
  }
  memcpy(p + o, b->root, 32); w64(p + o + 32, b->crc);
  if (nName) memcpy(p + o + PB_TRAILER, b->zName, nName);
  *pLen = len;
  return p;
}

int hpplite_pagebatch_meta(const unsigned char *p, size_t n, uint64_t *pTimestampMs, int *pMore) {
  if (!hpplite_pagebatch_is(p, n) || n < 64) return -1;
  if (pTimestampMs) *pTimestampMs = r64(p + 40);
  if (pMore) *pMore = r32(p + 4) == 1u ? 0 : (int)(r32(p + 60) & 1u);
  return 0;
}

HpplitePageBatch *hpplite_pagebatch_decode(const unsigned char *p, size_t n) {
  HpplitePageBatch *b;
  size_t rec, o;
  unsigned i, hdr;
  if (!hpplite_pagebatch_is(p, n)) return NULL;
  hdr = r32(p + 4) == 1u ? PB_HEADER_V1 : PB_HEADER;
  o = hdr;
  b = sqlite3_malloc64(sizeof(*b));
  if (!b) return NULL;
  memset(b, 0, sizeof(*b));
  b->pageSize = r32(p + 8); b->commitSize = r32(p + 12); b->firstCommit = r64(p + 16); b->lastCommit = r64(p + 24);
  b->salt1 = r32(p + 32); b->salt2 = r32(p + 36); b->timestampMs = r64(p + 40); b->nPages = r32(p + 48); b->nName = r32(p + 52);
  if (b->pageSize < 512 || b->pageSize > 65536 || (b->pageSize & (b->pageSize - 1)) || b->nName > 255) { sqlite3_free(b); return NULL; }
  rec = 6 + (size_t)b->pageSize;
  if (hdr == PB_HEADER) { b->piece = r32(p + 56); b->bMore = r32(p + 60) & 1u; }
  if (n != hdr + rec * b->nPages + PB_TRAILER + b->nName) { sqlite3_free(b); return NULL; }
  b->aPage = sqlite3_malloc64((sqlite3_uint64)(b->nPages ? b->nPages : 1) * sizeof(HpplitePageRec));
  if (!b->aPage) { sqlite3_free(b); return NULL; }
  for (i = 0; i < b->nPages; i++) {
    b->aPage[i].pgno = r32(p + o); b->aPage[i].flags = ((unsigned)p[o + 4] << 8) | p[o + 5]; b->aPage[i].body = p + o + 6;
    if (b->aPage[i].pgno == 0 || (i && b->aPage[i].pgno <= b->aPage[i - 1].pgno)) { hpplite_pagebatch_free(b); return NULL; }
    o += rec;
  }
  memcpy(b->root, p + o, 32); b->crc = r64(p + o + 32);
  b->zName = b->nName ? (const char *)(p + o + PB_TRAILER) : NULL;
  return b;
}

void hpplite_pagebatch_free(HpplitePageBatch *b) {
  if (!b) return;
  sqlite3_free(b->aPage); sqlite3_free(b);
}
