/*
** Ethereum keystore V3 (#17): the vendored primitives against their published
** vectors, then files written by cast (scrypt) and by hand (pbkdf2), a wrong
** passphrase, a tampered file. argv[1] is tests/fixtures.
*/
#include "test_lib.h"
#include "keystore.h"

static const char *g_fixtures;

static int hexeq(const uint8_t *a, const char *hex, size_t n) {
    char got[257];
    hpplite_bytes_to_hex(a, n, got);
    return strcmp(got, hex) == 0;
}

static void test_aes(void) {
    TEST("AES-128: FIPS-197 C.1 block; CTR round trip with a carrying counter");
    const uint8_t key[16] = { 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15 };
    const uint8_t pt[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    uint8_t ct[16], iv[16], a[40], b[40], c[40];
    int i;
    hpplite_aes128_encrypt_block(key, pt, ct);
    if (!hexeq(ct, "69c4e0d86a7b0430d8cdb78070b4c55a", 16)) FAIL("FIPS-197 C.1");
    memset(iv, 0xff, 16);                                     /* the counter carries across every byte */
    for (i = 0; i < 40; i++) a[i] = (uint8_t)(i * 7);
    hpplite_aes128_ctr(key, iv, a, 40, b);
    hpplite_aes128_ctr(key, iv, b, 40, c);
    if (memcmp(a, c, 40) != 0 || memcmp(a, b, 40) == 0) FAIL("CTR round trip");
    PASS();
}

static void test_pbkdf2(void) {
    TEST("PBKDF2-HMAC-SHA256: RFC 7914 section 11 vectors");
    uint8_t out[64];
    hpplite_pbkdf2_sha256((const uint8_t *)"passwd", 6, (const uint8_t *)"salt", 4, 1, out, 64);
    if (!hexeq(out, "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783", 64)) FAIL("c=1");
    hpplite_pbkdf2_sha256((const uint8_t *)"Password", 8, (const uint8_t *)"NaCl", 4, 80000, out, 64);
    if (!hexeq(out, "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d", 64)) FAIL("c=80000");
    PASS();
}

static void test_scrypt(void) {
    TEST("scrypt: RFC 7914 section 12 vectors (N=16 and N=1024, r=8, p=16); bad parameters refused");
    uint8_t out[64];
    if (hpplite_scrypt((const uint8_t *)"", 0, (const uint8_t *)"", 0, 16, 1, 1, out, 64) != 0) FAIL("N=16 ran");
    if (!hexeq(out, "77d6576238657b203b19ca42c18a0497f16b4844e3074ae8dfdffa3fede21442fcd0069ded0948f8326a753a0fc81f17e8d3e0fb2e0d3628cf35e20c38d18906", 64)) FAIL("N=16");
    if (hpplite_scrypt((const uint8_t *)"password", 8, (const uint8_t *)"NaCl", 4, 1024, 8, 16, out, 64) != 0) FAIL("N=1024 ran");
    if (!hexeq(out, "fdbabe1c9d3472007856e7190d01e9fe7c6ad7cbc8237830e77376634b3731622eaf30d92e22a3886ff109279d9830dac727afb94a83ee6d8360cbdfa2cc0640", 64)) FAIL("N=1024");
    if (hpplite_scrypt((const uint8_t *)"x", 1, (const uint8_t *)"y", 1, 1000, 8, 1, out, 32) == 0) FAIL("N not a power of two should be refused");
    if (hpplite_scrypt((const uint8_t *)"x", 1, (const uint8_t *)"y", 1, 1 << 22, 8, 1, out, 32) == 0) FAIL("N past the cap should be refused");
    PASS();
}

static void test_files(void) {
    TEST("a keystore written by cast (scrypt) and one by hand (pbkdf2) open to their keys; a wrong passphrase and a tampered MAC are refused with the reason");
    char path[600], err[200];
    unsigned char key[32], addr[20];
    HppliteCrypto *c = hpplite_crypto_init();
    HppliteKeypair kp;
    snprintf(path, sizeof path, "%s/keystore_scrypt.json", g_fixtures);
    if (hpplite_keystore_load(path, "test-pass", key, err, sizeof err) != 0) FAILF("scrypt file: %s", err);
    if (hpplite_crypto_keypair_from_privkey(c, &kp, key) != 0) FAIL("keypair");
    memcpy(addr, kp.address, 20);
    if (!hexeq(addr, "de9854a7873bbf6a04e3e1a6e9b2c3bc41fc2631", 20)) FAIL("the scrypt file's key is not cast's address 0xDE98...2631");
    if (hpplite_keystore_load(path, "wrong-pass", key, err, sizeof err) == 0 || !strstr(err, "MAC")) FAILF("a wrong passphrase should be refused by the MAC: %s", err);
    snprintf(path, sizeof path, "%s/keystore_pbkdf2.json", g_fixtures);
    if (hpplite_keystore_load(path, "pbkdf2-pass", key, err, sizeof err) != 0) FAILF("pbkdf2 file: %s", err);
    if (!hexeq(key, "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20", 32)) FAIL("the pbkdf2 file's key");
    {
        /* the same file with one byte of ciphertext changed: the MAC catches it */
        FILE *f = fopen(path, "rb");
        char buf[4096];
        size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
        char *p;
        if (f) fclose(f);
        buf[n] = 0;
        p = strstr(buf, "\"ciphertext\": \"");
        if (!p) FAIL("fixture shape");
        p += 15;
        p[0] = p[0] == 'a' ? 'b' : 'a';
        if (hpplite_keystore_decrypt(buf, "pbkdf2-pass", key, err, sizeof err) == 0 || !strstr(err, "MAC")) FAILF("a tampered ciphertext should be refused by the MAC: %s", err);
    }
    if (hpplite_keystore_decrypt("{\"version\": 2}", "x", key, err, sizeof err) == 0 || !strstr(err, "version 3")) FAIL("version 2 refused");
    if (hpplite_keystore_load(path, NULL, key, err, sizeof err) == 0 || !strstr(err, "passphrase")) FAIL("no passphrase refused with the reason");
    if (hpplite_keystore_load("/nonexistent/ks.json", "x", key, err, sizeof err) == 0) FAIL("missing file refused");
    hpplite_crypto_free(c);
    PASS();
}

int main(int argc, char **argv) {
#ifndef HPPLITE_FIXTURES_DIR
#define HPPLITE_FIXTURES_DIR "tests/fixtures"
#endif
    g_fixtures = argc > 1 ? argv[1] : HPPLITE_FIXTURES_DIR;
    printf("=== Keystore V3 (#17) ===\n");
    test_suite_init("keystore", NULL, NULL);
    test_aes();
    test_pbkdf2();
    test_scrypt();
    test_files();
    return test_report();
}
