/*
** The public-key envelope (#15): sealed to recipients' public keys, opened
** with a reader's private key alone; one and a hundred recipients; a key not
** among them, a tampered byte, another height all refused; the series' DEK.
*/
#include "test_lib.h"
#include "envelope_pk.h"

static void keyN(int i, uint8_t priv[32]) { memset(priv, 0, 32); priv[31] = (uint8_t)(i + 1); priv[30] = (uint8_t)(i >> 8); priv[0] = 0x3c; }

static void test_roundtrip(void) {
    TEST("sealed to one key: that key opens it, another does not (foreign), the DEK comes out, the payload is the AEAD's");
    uint8_t dek[32], priv[32], other[32], pub[33];
    const uint8_t payload[] = "rows for the reader";
    size_t n = 0, outLen = 0;
    int foreign = -1;
    memset(dek, 0x11, 32);
    keyN(1, priv); keyN(2, other);
    if (hpplite_pk_pubkey(priv, pub) != 0) FAIL("pubkey");
    uint8_t *env = hpplite_envelope_seal_pk(hpplite_cipher_default(), dek, pub, 1, 7, HPPLITE_ENCODING_NONE, payload, sizeof payload - 1, &n);
    if (!env) FAIL("seal");
    if (!hpplite_envelope_is_pk(env, n) || hpplite_envelope_is_sealed(env, n)) { free(env); FAIL("a v3 envelope is v3, not v2"); }
    if (n != sizeof payload - 1 + hpplite_envelope_pk_overhead(hpplite_cipher_default(), 1)) { free(env); FAIL("overhead"); }
    uint8_t got[32];
    uint8_t *pt = hpplite_envelope_open_pk(priv, 7, env, n, &outLen, &foreign, got);
    if (!pt || outLen != sizeof payload - 1 || memcmp(pt, payload, outLen) != 0 || memcmp(got, dek, 32) != 0) { free(pt); free(env); FAIL("open with the recipient's key"); }
    free(pt);
    pt = hpplite_envelope_open_pk(other, 7, env, n, &outLen, &foreign, NULL);
    if (pt || foreign != 1) { free(pt); free(env); FAIL("another key should not open it, and it is foreign (sealed to others)"); }
    pt = hpplite_envelope_open_pk(priv, 8, env, n, &outLen, &foreign, NULL);
    if (pt) { free(pt); free(env); FAIL("another height should not open it"); }
    env[n - 1] ^= 1;
    pt = hpplite_envelope_open_pk(priv, 7, env, n, &outLen, &foreign, NULL);
    if (pt) { free(pt); free(env); FAIL("a tampered payload should not open"); }
    env[n - 1] ^= 1;
    {
        /* a tampered wrap: the reader's own wrap fails, the envelope is "foreign" to it */
        size_t wrapAt = HPPLITE_ENVELOPE_MAGIC_LEN + 3 + 33 + 4 + 5;
        env[wrapAt] ^= 1;
        pt = hpplite_envelope_open_pk(priv, 7, env, n, &outLen, &foreign, NULL);
        if (pt) { free(pt); free(env); FAIL("a tampered wrap should not open"); }
        env[wrapAt] ^= 1;
    }
    free(env);
    PASS();
}

static void test_many(void) {
    TEST("sealed to 100 keys: each opens it; the fingerprints are listed; 256 recipients refused");
    uint8_t dek[32], pubs[100 * 33], privs[100][32], fps[100 * 4];
    const uint8_t payload[] = "shared with a hundred";
    size_t n = 0, outLen = 0;
    int i;
    memset(dek, 0x22, 32);
    for (i = 0; i < 100; i++) { keyN(10 + i, privs[i]); if (hpplite_pk_pubkey(privs[i], pubs + 33 * i) != 0) FAIL("pubkey"); }
    uint8_t *env = hpplite_envelope_seal_pk(hpplite_cipher_default(), dek, pubs, 100, 1, HPPLITE_ENCODING_DEFLATE, payload, sizeof payload - 1, &n);
    if (!env) FAIL("seal");
    if (hpplite_envelope_pk_encoding(env, n) != HPPLITE_ENCODING_DEFLATE) { free(env); FAIL("encoding byte"); }
    if (hpplite_envelope_pk_fingerprints(env, n, fps, 100) != 100) { free(env); FAIL("fingerprints"); }
    for (i = 0; i < 100; i++) {
        uint8_t fp[4];
        uint8_t *pt = hpplite_envelope_open_pk(privs[i], 1, env, n, &outLen, NULL, NULL);
        hpplite_pk_fingerprint(pubs + 33 * i, fp);
        if (!pt || memcmp(pt, payload, outLen) != 0 || memcmp(fps + 4 * i, fp, 4) != 0) { free(pt); free(env); FAIL("a recipient among a hundred"); }
        free(pt);
    }
    free(env);
    if (hpplite_envelope_seal_pk(hpplite_cipher_default(), dek, pubs, 256, 1, 0, payload, 1, &n) != NULL) FAIL("256 recipients should be refused");
    if (hpplite_envelope_seal_pk(hpplite_cipher_default(), dek, pubs, 0, 1, 0, payload, 1, &n) != NULL) FAIL("no recipient should be refused");
    PASS();
}

int main(void) {
    printf("=== Public-key envelope (#15) ===\n");
    test_suite_init("envelope_pk", NULL, NULL);
    test_roundtrip();
    test_many();
    return test_report();
}
