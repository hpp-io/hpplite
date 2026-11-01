/*
** HPPLite Crypto - secp256k1 signing implementation
*/

#include "crypto.h"
#include "hpplite_sqlite.h"
#include "keccak256.h"
#include "eth_client.h"
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <string.h>
#include <stdlib.h>

/*
** Crypto context structure
*/
struct HppliteCrypto {
    secp256k1_context *ctx;
};

/* Keccak-256 and hex: eth/keccak256.c and eth_client.c hold the one
** implementation of each (#50); these are the names the rest of HPPLite
** uses */
void hpplite_keccak256(const unsigned char *data, size_t len, unsigned char *hash_out) {
    keccak256(data, len, hash_out);
}

/*
** Initialize crypto context
*/
HppliteCrypto *hpplite_crypto_init(void) {
    HppliteCrypto *crypto = sqlite3_malloc(sizeof(HppliteCrypto));
    if (!crypto) return NULL;

    crypto->ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY
    );
    if (!crypto->ctx) {
        sqlite3_free(crypto);
        return NULL;
    }

    return crypto;
}

/*
** Free crypto context
*/
void hpplite_crypto_free(HppliteCrypto *crypto) {
    if (crypto) {
        if (crypto->ctx) {
            secp256k1_context_destroy(crypto->ctx);
        }
        sqlite3_free(crypto);
    }
}

/*
** Generate random keypair
*/
int hpplite_crypto_generate_keypair(HppliteCrypto *crypto, HppliteKeypair *kp) {
    if (!crypto || !kp) return -1;

    /* Generate random private key */
    sqlite3_randomness(HPPLITE_PRIVKEY_SIZE, kp->privkey);

    /* Derive public key and address */
    return hpplite_crypto_keypair_from_privkey(crypto, kp, kp->privkey);
}

/*
** Initialize keypair from private key
*/
int hpplite_crypto_keypair_from_privkey(
    HppliteCrypto *crypto,
    HppliteKeypair *kp,
    const unsigned char *privkey
) {
    secp256k1_pubkey pubkey;
    unsigned char pubkey_uncompressed[65];
    size_t pubkey_len;
    unsigned char hash[32];

    if (!crypto || !kp || !privkey) return -1;

    /* Copy private key */
    memcpy(kp->privkey, privkey, HPPLITE_PRIVKEY_SIZE);

    /* Verify private key is valid */
    if (!secp256k1_ec_seckey_verify(crypto->ctx, privkey)) {
        return -1;
    }

    /* Create public key */
    if (!secp256k1_ec_pubkey_create(crypto->ctx, &pubkey, privkey)) {
        return -1;
    }

    /* Serialize compressed public key */
    pubkey_len = HPPLITE_PUBKEY_SIZE;
    if (!secp256k1_ec_pubkey_serialize(crypto->ctx, kp->pubkey, &pubkey_len,
                                        &pubkey, SECP256K1_EC_COMPRESSED)) {
        return -1;
    }

    /* Serialize uncompressed for address derivation */
    pubkey_len = 65;
    if (!secp256k1_ec_pubkey_serialize(crypto->ctx, pubkey_uncompressed, &pubkey_len,
                                        &pubkey, SECP256K1_EC_UNCOMPRESSED)) {
        return -1;
    }

    /* Derive Ethereum address: keccak256(pubkey[1:65])[12:32] */
    hpplite_keccak256(pubkey_uncompressed + 1, 64, hash);
    memcpy(kp->address, hash + 12, HPPLITE_ADDRESS_SIZE);

    return 0;
}

/*
** Sign a message hash
*/
int hpplite_crypto_sign(
    HppliteCrypto *crypto,
    const HppliteKeypair *kp,
    const unsigned char *msg_hash,
    HppliteSignature *sig_out
) {
    secp256k1_ecdsa_recoverable_signature sig;
    int recid;

    if (!crypto || !kp || !msg_hash || !sig_out) return -1;

    /* Create recoverable signature */
    if (!secp256k1_ecdsa_sign_recoverable(crypto->ctx, &sig, msg_hash,
                                           kp->privkey, NULL, NULL)) {
        return -1;
    }

    /* Serialize to compact format */
    if (!secp256k1_ecdsa_recoverable_signature_serialize_compact(
            crypto->ctx, sig_out->sig, &recid, &sig)) {
        return -1;
    }

    sig_out->recid = recid;
    return 0;
}

/*
** Verify a signature
*/
int hpplite_crypto_verify(
    HppliteCrypto *crypto,
    const unsigned char *pubkey_bytes,
    const unsigned char *msg_hash,
    const HppliteSignature *sig
) {
    secp256k1_pubkey pubkey;
    secp256k1_ecdsa_signature sig_normal;
    secp256k1_ecdsa_recoverable_signature sig_rec;

    if (!crypto || !pubkey_bytes || !msg_hash || !sig) return -1;

    /* Parse public key */
    if (!secp256k1_ec_pubkey_parse(crypto->ctx, &pubkey,
                                    pubkey_bytes, HPPLITE_PUBKEY_SIZE)) {
        return -1;
    }

    /* Parse recoverable signature */
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            crypto->ctx, &sig_rec, sig->sig, sig->recid)) {
        return -1;
    }

    /* Convert to normal signature for verification */
    if (!secp256k1_ecdsa_recoverable_signature_convert(crypto->ctx, &sig_normal, &sig_rec)) {
        return -1;
    }

    /* Verify */
    return secp256k1_ecdsa_verify(crypto->ctx, &sig_normal, msg_hash, &pubkey);
}

/*
** Recover public key from signature
*/
int hpplite_crypto_recover_pubkey(
    HppliteCrypto *crypto,
    const unsigned char *msg_hash,
    const HppliteSignature *sig,
    unsigned char *pubkey_out
) {
    secp256k1_ecdsa_recoverable_signature sig_rec;
    secp256k1_pubkey pubkey;
    size_t pubkey_len = HPPLITE_PUBKEY_SIZE;

    if (!crypto || !msg_hash || !sig || !pubkey_out) return -1;

    /* Parse recoverable signature */
    if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
            crypto->ctx, &sig_rec, sig->sig, sig->recid)) {
        return -1;
    }

    /* Recover public key */
    if (!secp256k1_ecdsa_recover(crypto->ctx, &pubkey, &sig_rec, msg_hash)) {
        return -1;
    }

    /* Serialize */
    if (!secp256k1_ec_pubkey_serialize(crypto->ctx, pubkey_out, &pubkey_len,
                                        &pubkey, SECP256K1_EC_COMPRESSED)) {
        return -1;
    }

    return 0;
}

/*
** Hash a commitment for signing
*/
void hpplite_hash_commitment(
    uint64_t height,
    const unsigned char *state_root,
    const char *batch_ref,
    unsigned char *hash_out
) {
    /* Concatenate: height (8 bytes) || state_root (32 bytes) || batch_ref */
    size_t ref_len = batch_ref ? strlen(batch_ref) : 0;
    size_t total_len = 8 + 32 + ref_len;
    unsigned char *buf = sqlite3_malloc(total_len);

    if (!buf) {
        memset(hash_out, 0, 32);
        return;
    }

    /* Little-endian height */
    memcpy(buf, &height, 8);
    memcpy(buf + 8, state_root, 32);
    if (ref_len > 0) {
        memcpy(buf + 40, batch_ref, ref_len);
    }

    hpplite_keccak256(buf, total_len, hash_out);
    sqlite3_free(buf);
}

/*
** Convert signature to Ethereum format
*/
void hpplite_sig_to_eth_format(
    const HppliteSignature *sig,
    unsigned char *v,
    unsigned char *r,
    unsigned char *s
) {
    if (!sig) return;

    /* r = first 32 bytes */
    if (r) memcpy(r, sig->sig, 32);

    /* s = next 32 bytes */
    if (s) memcpy(s, sig->sig + 32, 32);

    /* v = 27 + recid (pre-EIP155) */
    if (v) *v = 27 + sig->recid;
}

/* Bytes to lowercase hex (no 0x), terminated; hex (0x or not) to bytes: the
** count, or -1 on an odd length, a bad digit or more than max_len bytes */
void hpplite_bytes_to_hex(const unsigned char *bytes, size_t len, char *hex_out) {
    eth_hex_encode_to(bytes, len, hex_out);
}

int hpplite_hex_to_bytes(const char *hex, unsigned char *bytes_out, size_t max_len) {
    return eth_hex_decode_any(hex, bytes_out, max_len);
}
