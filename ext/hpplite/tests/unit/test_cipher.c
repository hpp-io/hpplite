/*
** Batch encryption primitives against published test vectors, plus the
** envelope round trip.
*/

#include "test_lib.h"
#include "cipher.h"

static int hexeq(const uint8_t *b, const char *hex, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v; char s[3] = { hex[2*i], hex[2*i+1], 0 };
        if (sscanf(s, "%x", &v) != 1 || (uint8_t)v != b[i]) return 0;
    }
    return 1;
}

/* RFC 8439 section 2.8.2 */
static void test_rfc8439_aead(void) {
    const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
    uint8_t key[32], nonce[12] = {0x07,0,0,0,0x40,0x41,0x42,0x43,0x44,0x45,0x46,0x47};
    uint8_t ad[12] = {0x50,0x51,0x52,0x53,0xc0,0xc1,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7};
    uint8_t out[256], back[256];
    size_t n = strlen(pt);
    TEST("ChaCha20-Poly1305 RFC 8439 vector");
    for (int i = 0; i < 32; i++) key[i] = (uint8_t)(0x80 + i);
    const HppliteCipher *c = &HPPLITE_CIPHER_CHACHA20POLY1305;
    if (c->seal(key, nonce, ad, sizeof(ad), (const uint8_t*)pt, n, out) != 0) FAIL("seal");
    if (!hexeq(out, "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6", 32)) FAIL("ciphertext prefix");
    if (!hexeq(out + n, "1ae10b594f09e26a7e902ecbd0600691", 16)) FAIL("tag");
    if (c->open(key, nonce, ad, sizeof(ad), out, n + 16, back) != 0) FAIL("open");
    if (memcmp(back, pt, n) != 0) FAIL("round trip");
    out[5] ^= 1;
    if (c->open(key, nonce, ad, sizeof(ad), out, n + 16, back) == 0) FAIL("tampered ciphertext accepted");
    out[5] ^= 1; ad[0] ^= 1;
    if (c->open(key, nonce, ad, sizeof(ad), out, n + 16, back) == 0) FAIL("wrong associated data accepted");
    PASS();
}

/* RFC 5869 test case 1 */
static void test_rfc5869_hkdf(void) {
    uint8_t ikm[22], salt[13], info[10], okm[42];
    TEST("HKDF-SHA256 RFC 5869 vector");
    memset(ikm, 0x0b, sizeof(ikm));
    for (int i = 0; i < 13; i++) salt[i] = (uint8_t)i;
    for (int i = 0; i < 10; i++) info[i] = (uint8_t)(0xf0 + i);
    hpplite_hkdf_sha256(salt, sizeof(salt), ikm, sizeof(ikm), info, sizeof(info), okm, sizeof(okm));
    if (!hexeq(okm, "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865", 42)) FAIL("okm");
    PASS();
}

/* RFC 4231 test case 2 */
static void test_hmac(void) {
    uint8_t out[32];
    TEST("HMAC-SHA256 RFC 4231 vector");
    hpplite_hmac_sha256((const uint8_t*)"Jefe", 4, (const uint8_t*)"what do ya want for nothing?", 28, out);
    if (!hexeq(out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", 32)) FAIL("mac");
    PASS();
}

static void test_envelope(void) {
    uint8_t priv[32], key[32], key2[32];
    const char *payload = "{\"height\": 7, \"transactions\": []}";
    size_t sealedLen = 0, openLen = 0;
    TEST("envelope seal/open, key binding, height binding, legacy plaintext");
    for (int i = 0; i < 32; i++) priv[i] = (uint8_t)(i + 1);
    hpplite_cipher_derive_key(priv, key);
    priv[0] ^= 1; hpplite_cipher_derive_key(priv, key2);
    if (memcmp(key, key2, 32) == 0) FAIL("different private keys gave the same batch key");

    uint8_t *sealed = hpplite_envelope_seal(hpplite_cipher_default(), key, 7, HPPLITE_ENCODING_NONE,
                                            (const uint8_t*)payload, strlen(payload), &sealedLen);
    if (!sealed) FAIL("seal");
    if (!hpplite_envelope_is_sealed(sealed, sealedLen)) FAIL("not recognized as sealed");
    if (sealedLen != strlen(payload) + hpplite_envelope_overhead(hpplite_cipher_default())) FAIL("envelope size");
    if (memcmp(sealed, "HPE2", 4) != 0 || sealed[5] != HPPLITE_ENCODING_NONE) FAIL("v2 header");
    if (hpplite_envelope_encoding(sealed, sealedLen) != HPPLITE_ENCODING_NONE) FAIL("encoding byte");
    if (memmem(sealed, sealedLen, "transactions", 12)) FAIL("plaintext visible in envelope");

    const HppliteCipher *scheme = NULL;
    uint8_t *opened = hpplite_envelope_open(key, 7, sealed, sealedLen, &openLen, &scheme);
    if (!opened || openLen != strlen(payload) || strcmp((char*)opened, payload) != 0) FAIL("open");
    if (!scheme || strcmp(scheme->name, "chacha20poly1305") != 0) FAIL("scheme name");
    free(opened);
    if (hpplite_envelope_open(key2, 7, sealed, sealedLen, &openLen, NULL) != NULL) FAIL("wrong key accepted");
    if (hpplite_envelope_open(key, 8, sealed, sealedLen, &openLen, NULL) != NULL) FAIL("ciphertext moved to another height accepted");
    sealed[5] ^= 1;     /* encoding byte is authenticated */
    if (hpplite_envelope_open(key, 7, sealed, sealedLen, &openLen, NULL) != NULL) FAIL("tampered encoding byte accepted");
    sealed[5] ^= 1;
    sealed[4] = 0x7f;   /* unknown scheme id */
    if (hpplite_envelope_open(key, 7, sealed, sealedLen, &openLen, &scheme) != NULL || scheme != NULL) FAIL("unknown scheme accepted");
    free(sealed);

    /* a v1 (HPE1) envelope, as written by the v0.1 pre-release, still opens */
    {
        uint8_t v1[4 + 1 + 12 + 64], ad[9];
        const char *msg = "{\"height\":3}";
        size_t n = strlen(msg);
        memcpy(v1, "HPE1", 4); v1[4] = 1; memset(v1 + 5, 0x42, 12);
        ad[0] = 1; for (int i = 0; i < 8; i++) ad[1 + i] = (uint8_t)(3ULL >> (56 - 8 * i));
        HPPLITE_CIPHER_CHACHA20POLY1305.seal(key, v1 + 5, ad, 9, (const uint8_t*)msg, n, v1 + 17);
        if (!hpplite_envelope_is_sealed(v1, 17 + n + 16)) FAIL("v1 not recognized");
        if (hpplite_envelope_encoding(v1, 17 + n + 16) != HPPLITE_ENCODING_NONE) FAIL("v1 encoding");
        uint8_t *o = hpplite_envelope_open(key, 3, v1, 17 + n + 16, &openLen, NULL);
        if (!o || openLen != n || memcmp(o, msg, n) != 0) FAIL("v1 envelope does not open");
        free(o);
    }

    if (hpplite_envelope_is_sealed((const uint8_t*)payload, strlen(payload))) FAIL("plaintext JSON mistaken for an envelope");
    /* truncated envelopes of every short length are rejected, never read past the end */
    for (size_t n = 0; n <= 40; n++) {
        uint8_t tiny[48]; memcpy(tiny, "HPE2", 4); tiny[4] = 1; tiny[5] = 0; memset(tiny + 6, 0x11, sizeof(tiny) - 6);
        if (n >= 4 && hpplite_envelope_open(key, 1, tiny, n, &openLen, NULL) != NULL) FAIL("truncated envelope accepted");
        memcpy(tiny, "HPE1", 4);
        if (n >= 4 && hpplite_envelope_open(key, 1, tiny, n, &openLen, NULL) != NULL) FAIL("truncated v1 envelope accepted");
    }
    /* two seals of the same payload differ (fresh nonce) */
    uint8_t *s1 = hpplite_envelope_seal(hpplite_cipher_default(), key, 1, HPPLITE_ENCODING_NONE, (const uint8_t*)payload, strlen(payload), &sealedLen);
    uint8_t *s2 = hpplite_envelope_seal(hpplite_cipher_default(), key, 1, HPPLITE_ENCODING_NONE, (const uint8_t*)payload, strlen(payload), &openLen);
    if (!s1 || !s2 || memcmp(s1, s2, sealedLen) == 0) FAIL("nonce reused");
    free(s1); free(s2);
    PASS();
}

int main(void) {
    printf("=== HPPLite Cipher Tests ===\n");
    test_rfc8439_aead();
    test_rfc5869_hkdf();
    test_hmac();
    test_envelope();
    return test_report();
}
