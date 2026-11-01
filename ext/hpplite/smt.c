/*
** HPPLite - sparse Merkle tree (#9). See smt.h for the shape and the hashes.
*/
#include "smt.h"
#include "sha256.h"
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ paths */

typedef struct Path {
  int depth;                              /* 0..256 */
  uint8_t prefix[HPPLITE_SMT_HASH];       /* the first depth bits of the key, rest zero */
} Path;

static int keyBit(const uint8_t *key, int i) { return (key[i >> 3] >> (7 - (i & 7))) & 1; }

static void pathBytes(const Path *p, uint8_t *out) {
  out[0] = (uint8_t)(p->depth >> 8);
  out[1] = (uint8_t)p->depth;
  memcpy(out + 2, p->prefix, HPPLITE_SMT_HASH);
}

/* the child of p on bit b */
static void pathChild(const Path *p, int b, Path *out) {
  *out = *p;
  if (b) out->prefix[p->depth >> 3] |= (uint8_t)(0x80 >> (p->depth & 7));
  out->depth = p->depth + 1;
}

/* the path of key at depth d */
static void pathOf(const uint8_t *key, int d, Path *out) {
  int i;
  memset(out, 0, sizeof *out);
  out->depth = d;
  for (i = 0; i < (d >> 3); i++) out->prefix[i] = key[i];
  if (d & 7) out->prefix[d >> 3] = key[d >> 3] & (uint8_t)(0xff << (8 - (d & 7)));
}

/* first bit at which a and b differ (256 when equal) */
static int firstDiff(const uint8_t *a, const uint8_t *b) {
  int i, j;
  for (i = 0; i < HPPLITE_SMT_HASH; i++) {
    uint8_t x = a[i] ^ b[i];
    if (x) {
      for (j = 0; j < 8; j++) if (x & (0x80 >> j)) return i * 8 + j;
    }
  }
  return 256;
}

/* ----------------------------------------------------------------- hashes */

void hpplite_smt_leaf_hash(const uint8_t *key, const uint8_t *valueHash, uint8_t *out) {
  SHA256_CTX h;
  hpplite_sha256_init(&h);
  hpplite_sha256_update(&h, (const uint8_t *)"L", 1);
  hpplite_sha256_update(&h, key, HPPLITE_SMT_HASH);
  hpplite_sha256_update(&h, valueHash, HPPLITE_SMT_HASH);
  hpplite_sha256_final(&h, out);
}

static void innerHash(const uint8_t *l, const uint8_t *r, uint8_t *out) {
  SHA256_CTX h;
  hpplite_sha256_init(&h);
  hpplite_sha256_update(&h, (const uint8_t *)"N", 1);
  hpplite_sha256_update(&h, l, HPPLITE_SMT_HASH);
  hpplite_sha256_update(&h, r, HPPLITE_SMT_HASH);
  hpplite_sha256_final(&h, out);
}

/* the hash a node contributes to its parent */
static void nodeHash(const HppliteSmtNode *n, uint8_t *out) {
  if (n->kind == HPPLITE_SMT_LEAF) hpplite_smt_leaf_hash(n->key, n->hash, out);
  else if (n->kind == HPPLITE_SMT_INNER) memcpy(out, n->hash, HPPLITE_SMT_HASH);
  else memset(out, 0, HPPLITE_SMT_HASH);
}

/* ------------------------------------------------------------ store calls */

static int getNode(const HppliteSmtStore *s, const char *zTab, const Path *p, HppliteSmtNode *n) {
  uint8_t a[HPPLITE_SMT_PATH_LEN];
  pathBytes(p, a);
  memset(n, 0, sizeof *n);
  return s->xGet(s->pArg, zTab, a, n);
}

static int putNode(const HppliteSmtStore *s, const char *zTab, const Path *p, const HppliteSmtNode *n) {
  uint8_t a[HPPLITE_SMT_PATH_LEN];
  pathBytes(p, a);
  return s->xPut(s->pArg, zTab, a, n);
}

static int putLeaf(const HppliteSmtStore *s, const char *zTab, const Path *p, const uint8_t *key, const uint8_t *vh) {
  HppliteSmtNode n;
  n.kind = HPPLITE_SMT_LEAF;
  memcpy(n.key, key, HPPLITE_SMT_HASH);
  memcpy(n.hash, vh, HPPLITE_SMT_HASH);
  return putNode(s, zTab, p, &n);
}

/* Rehash the inner node at p from its children, collapsing it to a leaf when
** only one leaf is left beneath it (the canonical form). Returns the node's
** new kind, or <0. */
static int rehash(const HppliteSmtStore *s, const char *zTab, const Path *p) {
  Path l, r;
  HppliteSmtNode nl, nr, n;
  uint8_t hl[HPPLITE_SMT_HASH], hr[HPPLITE_SMT_HASH];
  int rc;
  pathChild(p, 0, &l);
  pathChild(p, 1, &r);
  if ((rc = getNode(s, zTab, &l, &nl)) < 0) return rc;
  if ((rc = getNode(s, zTab, &r, &nr)) < 0) return rc;
  if (nl.kind == HPPLITE_SMT_EMPTY && nr.kind == HPPLITE_SMT_EMPTY) {
    if ((rc = putNode(s, zTab, p, NULL)) < 0) return rc;
    return HPPLITE_SMT_EMPTY;
  }
  if (nl.kind == HPPLITE_SMT_LEAF && nr.kind == HPPLITE_SMT_EMPTY) {
    if ((rc = putNode(s, zTab, p, &nl)) < 0) return rc;
    if ((rc = putNode(s, zTab, &l, NULL)) < 0) return rc;
    return HPPLITE_SMT_LEAF;
  }
  if (nr.kind == HPPLITE_SMT_LEAF && nl.kind == HPPLITE_SMT_EMPTY) {
    if ((rc = putNode(s, zTab, p, &nr)) < 0) return rc;
    if ((rc = putNode(s, zTab, &r, NULL)) < 0) return rc;
    return HPPLITE_SMT_LEAF;
  }
  nodeHash(&nl, hl);
  nodeHash(&nr, hr);
  memset(&n, 0, sizeof n);
  n.kind = HPPLITE_SMT_INNER;
  innerHash(hl, hr, n.hash);
  if ((rc = putNode(s, zTab, p, &n)) < 0) return rc;
  return HPPLITE_SMT_INNER;
}

int hpplite_smt_set(const HppliteSmtStore *s, const char *zTab, const uint8_t *key, const uint8_t *vh) {
  Path p;
  int depth, rc;
  int stackTop = 0;                       /* depths of the inner nodes on the way down: 0..stackTop-1 */
  pathOf(key, 0, &p);
  for (depth = 0; depth <= 256; depth++) {
    HppliteSmtNode n;
    pathOf(key, depth, &p);
    if ((rc = getNode(s, zTab, &p, &n)) < 0) return rc;
    if (n.kind == HPPLITE_SMT_EMPTY) {
      if (vh && (rc = putLeaf(s, zTab, &p, key, vh)) < 0) return rc;
      break;
    }
    if (n.kind == HPPLITE_SMT_LEAF) {
      if (memcmp(n.key, key, HPPLITE_SMT_HASH) == 0) {
        rc = vh ? putLeaf(s, zTab, &p, key, vh) : putNode(s, zTab, &p, NULL);
        if (rc < 0) return rc;
      } else if (vh) {
        /* two keys below this point: inner nodes down to the bit they differ at */
        int d, split = firstDiff(n.key, key);
        Path q;
        for (d = depth; d <= split; d++) {
          HppliteSmtNode inner;
          memset(&inner, 0, sizeof inner);
          inner.kind = HPPLITE_SMT_INNER;   /* hash set on the way back up */
          pathOf(key, d, &q);
          if ((rc = putNode(s, zTab, &q, &inner)) < 0) return rc;
        }
        pathOf(n.key, split + 1, &q);
        if ((rc = putLeaf(s, zTab, &q, n.key, n.hash)) < 0) return rc;
        pathOf(key, split + 1, &q);
        if ((rc = putLeaf(s, zTab, &q, key, vh)) < 0) return rc;
        stackTop = split + 1;             /* rehash depths split..0 */
        break;
      }
      /* clearing a key that is not there: nothing */
      break;
    }
    stackTop = depth + 1;                 /* an inner node: continue below it */
  }
  /* back up: every inner node on the way gets its hash, or collapses */
  for (depth = stackTop - 1; depth >= 0; depth--) {
    pathOf(key, depth, &p);
    if ((rc = rehash(s, zTab, &p)) < 0) return rc;
  }
  return 0;
}

int hpplite_smt_root(const HppliteSmtStore *s, const char *zTab, uint8_t *out) {
  Path p;
  HppliteSmtNode n;
  int rc;
  memset(&p, 0, sizeof p);
  if ((rc = getNode(s, zTab, &p, &n)) < 0) return rc;
  nodeHash(&n, out);
  return 0;
}

/* ------------------------------------------------------ build from a list */

int hpplite_smt_leaf_cmp(const void *a, const void *b) {
  return memcmp(((const HppliteSmtLeaf *)a)->key, ((const HppliteSmtLeaf *)b)->key, HPPLITE_SMT_HASH);
}

static void buildRoot(const HppliteSmtLeaf *a, size_t lo, size_t hi, int depth, uint8_t *out) {
  size_t mid, l, r;
  uint8_t hl[HPPLITE_SMT_HASH], hr[HPPLITE_SMT_HASH];
  if (hi - lo == 0) { memset(out, 0, HPPLITE_SMT_HASH); return; }
  if (hi - lo == 1) { hpplite_smt_leaf_hash(a[lo].key, a[lo].hash, out); return; }
  /* sorted: the keys with bit depth set come after those without */
  l = lo; r = hi;
  while (l < r) {
    mid = l + (r - l) / 2;
    if (keyBit(a[mid].key, depth)) r = mid; else l = mid + 1;
  }
  mid = l;
  buildRoot(a, lo, mid, depth + 1, hl);
  buildRoot(a, mid, hi, depth + 1, hr);
  innerHash(hl, hr, out);
}

void hpplite_smt_build_root(const HppliteSmtLeaf *aLeaf, size_t n, uint8_t *out) {
  buildRoot(aLeaf, 0, n, 0, out);
}

static int buildStore(const HppliteSmtStore *s, const char *zTab, const HppliteSmtLeaf *a,
                      size_t lo, size_t hi, const Path *p, uint8_t *out) {
  size_t mid, l, r;
  Path pl, pr;
  HppliteSmtNode n;
  uint8_t hl[HPPLITE_SMT_HASH], hr[HPPLITE_SMT_HASH];
  int rc;
  if (hi - lo == 0) { memset(out, 0, HPPLITE_SMT_HASH); return 0; }
  if (hi - lo == 1) {
    hpplite_smt_leaf_hash(a[lo].key, a[lo].hash, out);
    return putLeaf(s, zTab, p, a[lo].key, a[lo].hash);
  }
  l = lo; r = hi;
  while (l < r) {
    mid = l + (r - l) / 2;
    if (keyBit(a[mid].key, p->depth)) r = mid; else l = mid + 1;
  }
  mid = l;
  pathChild(p, 0, &pl);
  pathChild(p, 1, &pr);
  if ((rc = buildStore(s, zTab, a, lo, mid, &pl, hl)) < 0) return rc;
  if ((rc = buildStore(s, zTab, a, mid, hi, &pr, hr)) < 0) return rc;
  memset(&n, 0, sizeof n);
  n.kind = HPPLITE_SMT_INNER;
  innerHash(hl, hr, n.hash);
  memcpy(out, n.hash, HPPLITE_SMT_HASH);
  return putNode(s, zTab, p, &n);
}

int hpplite_smt_build_store(const HppliteSmtStore *s, const char *zTab, const HppliteSmtLeaf *aLeaf, size_t n, uint8_t *out) {
  Path p;
  memset(&p, 0, sizeof p);
  return buildStore(s, zTab, aLeaf, 0, n, &p, out);
}

/* --------------------------------------------------------- memory store */

typedef struct MemNode {
  struct MemNode *pNext;
  char *zTab;
  uint8_t aPath[HPPLITE_SMT_PATH_LEN];
  HppliteSmtNode node;
} MemNode;

typedef struct MemStore {
  MemNode **aBucket;
  size_t nBucket, nNode;
} MemStore;

static size_t memHash(const char *zTab, const uint8_t *aPath) {
  size_t h = 1469598103934665603u;
  int i;
  for (; *zTab; zTab++) h = (h ^ (uint8_t)*zTab) * 1099511628211u;
  for (i = 0; i < HPPLITE_SMT_PATH_LEN; i++) h = (h ^ aPath[i]) * 1099511628211u;
  return h;
}

static MemNode **memFind(MemStore *m, const char *zTab, const uint8_t *aPath) {
  MemNode **pp = &m->aBucket[memHash(zTab, aPath) % m->nBucket];
  while (*pp) {
    if (memcmp((*pp)->aPath, aPath, HPPLITE_SMT_PATH_LEN) == 0 && strcmp((*pp)->zTab, zTab) == 0) break;
    pp = &(*pp)->pNext;
  }
  return pp;
}

static int memGet(void *pArg, const char *zTab, const uint8_t *aPath, HppliteSmtNode *pNode) {
  MemNode **pp = memFind((MemStore *)pArg, zTab, aPath);
  if (*pp) *pNode = (*pp)->node; else pNode->kind = HPPLITE_SMT_EMPTY;
  return 0;
}

static int memPut(void *pArg, const char *zTab, const uint8_t *aPath, const HppliteSmtNode *pNode) {
  MemStore *m = (MemStore *)pArg;
  MemNode **pp = memFind(m, zTab, aPath);
  if (!pNode) {
    if (*pp) { MemNode *d = *pp; *pp = d->pNext; free(d->zTab); free(d); m->nNode--; }
    return 0;
  }
  if (!*pp) {
    MemNode *n = calloc(1, sizeof *n);
    if (!n) return -1;
    n->zTab = strdup(zTab);
    if (!n->zTab) { free(n); return -1; }
    memcpy(n->aPath, aPath, HPPLITE_SMT_PATH_LEN);
    *pp = n;
    m->nNode++;
  }
  (*pp)->node = *pNode;
  return 0;
}

HppliteSmtStore *hpplite_smt_mem_store(void) {
  HppliteSmtStore *s = calloc(1, sizeof *s);
  MemStore *m = calloc(1, sizeof *m);
  if (!s || !m) { free(s); free(m); return NULL; }
  m->nBucket = 4096;
  m->aBucket = calloc(m->nBucket, sizeof *m->aBucket);
  if (!m->aBucket) { free(s); free(m); return NULL; }
  s->pArg = m;
  s->xGet = memGet;
  s->xPut = memPut;
  return s;
}

void hpplite_smt_mem_store_free(HppliteSmtStore *s) {
  MemStore *m;
  size_t i;
  if (!s) return;
  m = (MemStore *)s->pArg;
  for (i = 0; i < m->nBucket; i++) {
    MemNode *n = m->aBucket[i];
    while (n) { MemNode *next = n->pNext; free(n->zTab); free(n); n = next; }
  }
  free(m->aBucket);
  free(m);
  free(s);
}

size_t hpplite_smt_mem_store_count(const HppliteSmtStore *s) {
  return s ? ((const MemStore *)s->pArg)->nNode : 0;
}

int hpplite_smt_mem_store_lookup(const HppliteSmtStore *s, const char *zTab, const uint8_t *aPath, HppliteSmtNode *pNode) {
  MemNode **pp = memFind((MemStore *)s->pArg, zTab, aPath);
  if (!*pp) return 0;
  *pNode = (*pp)->node;
  return 1;
}

int hpplite_smt_mem_store_each(const HppliteSmtStore *s, const char *zTab,
                               int (*xEach)(void *, const char *, const uint8_t *, const HppliteSmtNode *), void *pArg) {
  const MemStore *m = (const MemStore *)s->pArg;
  size_t i;
  for (i = 0; i < m->nBucket; i++) {
    MemNode *n;
    for (n = m->aBucket[i]; n; n = n->pNext) {
      int rc;
      if (zTab && strcmp(n->zTab, zTab) != 0) continue;
      if ((rc = xEach(pArg, n->zTab, n->aPath, &n->node)) != 0) return rc;
    }
  }
  return 0;
}

void hpplite_smt_mem_store_clear(HppliteSmtStore *s, const char *zTab) {
  MemStore *m = (MemStore *)s->pArg;
  size_t i;
  for (i = 0; i < m->nBucket; i++) {
    MemNode **pp = &m->aBucket[i];
    while (*pp) {
      if (strcmp((*pp)->zTab, zTab) == 0) { MemNode *d = *pp; *pp = d->pNext; free(d->zTab); free(d); m->nNode--; }
      else pp = &(*pp)->pNext;
    }
  }
}
