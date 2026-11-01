/*
** HPPLite - a sparse Merkle tree over 256-bit keys (#9): the per-table
** structure of the version 2 state root.
**
** Shape is a pure function of the key set. A subtree holding exactly one
** key is that key's leaf, wherever it sits; a subtree holding two or more is
** an inner node splitting on the next bit of the key; an empty subtree
** hashes to 32 zero bytes. Hashes:
**
**   leaf  = SHA256("L" || key || valueHash)
**   inner = SHA256("N" || hash(left) || hash(right))
**
** The leaf hash does not include its depth, so a leaf moving up or down as
** neighbours come and go is not rehashed, and the same key set in any order
** of operations gives the same tree. A set or clear touches about log2(n)
** nodes. hpplite_smt_build_root() computes the root of a sorted leaf list
** without a store, in O(n) hashes: what hpplite_db_hash() does on a plain
** file.
**
** Nodes live in a store the caller provides, addressed by path: a 2-byte
** big-endian depth (0..256) and the key prefix of that many bits, the rest
** of the 32 prefix bytes zero. A node is HPPLITE_SMT_EMPTY (absent),
** HPPLITE_SMT_LEAF (key, valueHash) or HPPLITE_SMT_INNER (its hash).
*/
#ifndef HPPLITE_SMT_H
#define HPPLITE_SMT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HPPLITE_SMT_HASH 32
#define HPPLITE_SMT_PATH_LEN 34           /* depth (2 bytes) + 32 prefix bytes */

#define HPPLITE_SMT_EMPTY 0
#define HPPLITE_SMT_LEAF 1
#define HPPLITE_SMT_INNER 2

typedef struct HppliteSmtNode {
  int kind;                               /* HPPLITE_SMT_* */
  uint8_t key[HPPLITE_SMT_HASH];          /* leaf: its key */
  uint8_t hash[HPPLITE_SMT_HASH];         /* leaf: the value hash; inner: the node hash */
} HppliteSmtNode;

/* The node store. get: 0 and *pNode filled (kind EMPTY when absent), <0 on
** error. put: pNode NULL removes the node; <0 on error. zTab names the tree. */
typedef struct HppliteSmtStore {
  void *pArg;
  int (*xGet)(void *pArg, const char *zTab, const uint8_t *aPath, HppliteSmtNode *pNode);
  int (*xPut)(void *pArg, const char *zTab, const uint8_t *aPath, const HppliteSmtNode *pNode);
} HppliteSmtStore;

/* Set the leaf of key to valueHash, or clear it (valueHash NULL). 0, or the store's error */
int hpplite_smt_set(const HppliteSmtStore *pStore, const char *zTab,
                    const uint8_t key[HPPLITE_SMT_HASH], const uint8_t *valueHash);

/* The tree's root hash (zeros when empty) */
int hpplite_smt_root(const HppliteSmtStore *pStore, const char *zTab, uint8_t out[HPPLITE_SMT_HASH]);

/* The leaf hash of (key, valueHash) */
void hpplite_smt_leaf_hash(const uint8_t key[HPPLITE_SMT_HASH], const uint8_t valueHash[HPPLITE_SMT_HASH],
                           uint8_t out[HPPLITE_SMT_HASH]);

/* The root of n leaves sorted by key (distinct keys), without a store */
typedef struct HppliteSmtLeaf {
  uint8_t key[HPPLITE_SMT_HASH];
  uint8_t hash[HPPLITE_SMT_HASH];
} HppliteSmtLeaf;
void hpplite_smt_build_root(const HppliteSmtLeaf *aLeaf, size_t n, uint8_t out[HPPLITE_SMT_HASH]);
int hpplite_smt_leaf_cmp(const void *a, const void *b);     /* qsort order of HppliteSmtLeaf by key */
/* The same n sorted leaves put into a store as the tree's nodes (the tree
** must be empty there); its root in out */
int hpplite_smt_build_store(const HppliteSmtStore *pStore, const char *zTab,
                            const HppliteSmtLeaf *aLeaf, size_t n, uint8_t out[HPPLITE_SMT_HASH]);

/* An in-memory store (tests, scratch trees). */
HppliteSmtStore *hpplite_smt_mem_store(void);
void hpplite_smt_mem_store_free(HppliteSmtStore *pStore);
size_t hpplite_smt_mem_store_count(const HppliteSmtStore *pStore);    /* nodes held */
/* Is a node held at this path? 1 and *pNode (which may be kind EMPTY, a
** recorded absence), 0 when nothing was put there */
int hpplite_smt_mem_store_lookup(const HppliteSmtStore *pStore, const char *zTab, const uint8_t *aPath, HppliteSmtNode *pNode);
/* Every node of one table, or of every table (zTab NULL), to a callback; stops at its first nonzero */
int hpplite_smt_mem_store_each(const HppliteSmtStore *pStore, const char *zTab,
                               int (*xEach)(void *pArg, const char *zTab, const uint8_t *aPath, const HppliteSmtNode *pNode), void *pArg);
/* Forget every node of one table */
void hpplite_smt_mem_store_clear(HppliteSmtStore *pStore, const char *zTab);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_SMT_H */
