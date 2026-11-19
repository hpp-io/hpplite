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

    /* the v0.1 pre-release layout "HPE1" (no encoding byte) is no envelope since v0.8 (#249): no tagged release wrote
    ** it. It is neither recognized as sealed nor opened - it reads as a plaintext payload and fails as JSON there */
    {
        uint8_t v1[4 + 1 + 12 + 64], ad[9];
        const char *msg = "{\"height\":3}";
        size_t n = strlen(msg);
        memcpy(v1, "HPE1", 4); v1[4] = 1; memset(v1 + 5, 0x42, 12);
        ad[0] = 1; for (int i = 0; i < 8; i++) ad[1 + i] = (uint8_t)(3ULL >> (56 - 8 * i));
        HPPLITE_CIPHER_CHACHA20POLY1305.seal(key, v1 + 5, ad, 9, (const uint8_t*)msg, n, v1 + 17);
        if (hpplite_envelope_is_sealed(v1, 17 + n + 16)) FAIL("HPE1 still recognized as an envelope");
        if (hpplite_envelope_encoding(v1, 17 + n + 16) != -1) FAIL("HPE1 given an encoding");
        if (hpplite_envelope_open(key, 3, v1, 17 + n + 16, &openLen, NULL) != NULL) FAIL("HPE1 still opens");
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

/* A streamed seal (#364, a snapshot never whole in memory): the payload in pieces of uneven sizes, some across
** ChaCha20's 64-byte blocks, one empty - the envelope hpplite_envelope_open reads, as long as the copying seal's */
static void test_seal_stream(void) {
    TEST("an envelope sealed as a stream, in uneven pieces, opens to its payload at the copying seal's length (#364)");
    const HppliteCipher *c = hpplite_cipher_default();
    uint8_t key[32], hdr[64], tag[16];
    size_t n = 100003, hl = 0, off = 0, openLen = 0, refLen = 0, piece = 1;
    uint8_t *payload = malloc(n), *env = malloc(64 + n + 16), *ref, *opened;
    HppliteSealStream *st;
    for (size_t i = 0; i < 32; i++) key[i] = (uint8_t)(i * 7 + 1);
    for (size_t i = 0; i < n; i++) payload[i] = (uint8_t)(i * 31 + (i >> 8));
    st = hpplite_seal_stream_new(c, key, 9, HPPLITE_ENCODING_DEFLATE, hdr, &hl);
    if (!st) { free(payload); free(env); FAIL("stream"); }
    memcpy(env, hdr, hl);
    hpplite_seal_stream_update(st, payload, env + hl, 0);
    while (off < n) {
        size_t k = piece < n - off ? piece : n - off;
        hpplite_seal_stream_update(st, payload + off, env + hl + off, k);
        off += k; piece = piece * 3 + 1;
    }
    hpplite_seal_stream_final(st, tag);
    memcpy(env + hl + n, tag, 16);
    ref = hpplite_envelope_seal(c, key, 9, HPPLITE_ENCODING_DEFLATE, payload, n, &refLen);
    opened = hpplite_envelope_open(key, 9, env, hl + n + 16, &openLen, NULL);
    int ok = opened && openLen == n && memcmp(opened, payload, n) == 0 && ref && refLen == hl + n + 16
             && hpplite_envelope_encoding(env, hl + n + 16) == HPPLITE_ENCODING_DEFLATE
             && hpplite_envelope_open(key, 10, env, hl + n + 16, &openLen, NULL) == NULL;
    free(opened); free(ref); free(payload); free(env);
    if (!ok) FAIL("the streamed envelope is not the one hpplite_envelope_seal makes");
    PASS();
}

/* Opened as a stream (#364, a snapshot read from a file): the tag checked in a MAC-only pass, then opened in uneven
** pieces back to the payload; one flipped byte fails the tag */
static void test_open_stream(void) {
    TEST("an envelope opened as a stream: the MAC pass takes its tag, the pieces open to the payload, a flipped byte fails (#364)");
    const HppliteCipher *c = hpplite_cipher_default();
    uint8_t key[32];
    size_t n = 70001, len = 0, hl = 0, off, piece;
    int enc = -1;
    uint8_t *payload = malloc(n), *env, *out = malloc(n);
    HppliteSealStream *st;
    int ok = 1;
    for (size_t i = 0; i < 32; i++) key[i] = (uint8_t)(i + 3);
    for (size_t i = 0; i < n; i++) payload[i] = (uint8_t)(i * 7);
    env = hpplite_envelope_seal(c, key, 5, HPPLITE_ENCODING_DEFLATE, payload, n, &len);
    st = env ? hpplite_envelope_open_stream(key, 5, env, len, len, &hl, &enc, NULL) : NULL;
    if (!st || enc != HPPLITE_ENCODING_DEFLATE) { free(env); free(payload); free(out); FAIL("open the stream"); }
    hpplite_open_stream_mac(st, env + hl, len - hl - 16);
    ok = hpplite_open_stream_final(st, env + len - 16) == 0;
    st = hpplite_envelope_open_stream(key, 5, env, len, len, &hl, &enc, NULL);
    for (off = 0, piece = 1; st && off < n; off += piece, piece = piece * 5 + 3) {
        if (piece > n - off) piece = n - off;
        hpplite_open_stream_update(st, env + hl + off, out + off, piece);
    }
    ok = ok && st && hpplite_open_stream_final(st, env + len - 16) == 0 && memcmp(out, payload, n) == 0;
    env[hl + 100] ^= 1;
    st = hpplite_envelope_open_stream(key, 5, env, len, len, &hl, &enc, NULL);
    if (st) { hpplite_open_stream_mac(st, env + hl, len - hl - 16); ok = ok && hpplite_open_stream_final(st, env + len - 16) != 0; }
    free(env); free(payload); free(out);
    if (!ok) FAIL("the streamed open does not match the seal");
    PASS();
}

int main(void) {
    printf("=== HPPLite Cipher Tests ===\n");
    test_rfc8439_aead();
    test_rfc5869_hkdf();
    test_hmac();
    test_envelope();
    test_seal_stream();
    test_open_stream();
    return test_report();
}
