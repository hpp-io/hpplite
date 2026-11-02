/*
** The public-key envelope, "HPE3" (#15): a batch sealed to recipients'
** public keys. One random data key (DEK) per batch series seals the payload
** with the AEAD of cipher.c; the DEK is wrapped once per recipient with an
** ephemeral secp256k1 key (ECDH, HKDF-SHA256, the same AEAD), so a reader
** needs only its own key and nothing else from the writer: a viewing key
** reads without the power to act, and a grant or revoke is a change to the
** next series' recipients, never a re-encryption of history.
**
**   "HPE3" | scheme (1) | encoding (1) | n (1) | ephemeral pubkey (33)
**         | n x [ fingerprint (4) | wrapped DEK (32 + tag) ] | nonce | ciphertext || tag
**
** fingerprint = keccak256(compressed recipient pubkey)[0..4]: a reader tries
** the wraps whose fingerprint is its own (a collision costs a failed unwrap).
** Associated data of the payload: scheme | encoding | height (8) | ephemeral
** pubkey; of each wrap: the fingerprint and the height.
*/
#ifndef HPPLITE_ENVELOPE_PK_H
#define HPPLITE_ENVELOPE_PK_H

#include <stddef.h>
#include <stdint.h>
#include "cipher.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HPPLITE_ENVELOPE_MAGIC_V3 "HPE3"
#define HPPLITE_PK_PUBKEY_LEN 33
#define HPPLITE_PK_FP_LEN 4
#define HPPLITE_PK_MAX_RECIPIENTS 255

/* keccak256(pubkey)[0..4] */
void hpplite_pk_fingerprint(const uint8_t pubkey[33], uint8_t fp[4]);

/* The compressed public key of a private key: 0, or -1 when it is not a key */
int hpplite_pk_pubkey(const uint8_t privkey[32], uint8_t pubkey[33]);

/* Seal payload under dek to the n recipient public keys (33 bytes each). malloc'd envelope and
** its length, or NULL (no recipients, too many, a bad key, no randomness) */
uint8_t *hpplite_envelope_seal_pk(const HppliteCipher *cipher, const uint8_t dek[32],
                                  const uint8_t *aRecipients, int nRecipients,
                                  uint64_t height, uint8_t encoding,
                                  const uint8_t *payload, size_t payloadLen, size_t *pOutLen);

/* Open a v3 envelope with a reader's private key: malloc'd, terminated payload, or NULL when
** no wrap is for this key (*pForeign = 1: the envelope is well-formed and sealed to others) or
** the data is malformed. pDek, when given, receives the series' DEK (a reader keeps it). */
uint8_t *hpplite_envelope_open_pk(const uint8_t privkey[32], uint64_t height,
                                  const uint8_t *data, size_t len, size_t *pOutLen,
                                  int *pForeign, uint8_t *pDek);

/* Is this a v3 envelope? The bytes an envelope adds around the payload for n recipients. */
int hpplite_envelope_is_pk(const uint8_t *data, size_t len);
size_t hpplite_envelope_pk_overhead(const HppliteCipher *cipher, int nRecipients);
/* The encoding byte of a v3 envelope, or -1 */
int hpplite_envelope_pk_encoding(const uint8_t *data, size_t len);
/* The recipients' fingerprints of a v3 envelope: the count (into out, up to max), or -1 */
int hpplite_envelope_pk_fingerprints(const uint8_t *data, size_t len, uint8_t *out, int max);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_ENVELOPE_PK_H */
