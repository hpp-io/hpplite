/*
** The node's signatures against operators/conformance/vectors.json (#342): every store message (Bind, Release, Delete,
** Cap, Content, Prune, Unpin, BucketStatement) in ("HPPLiteStore", "2", chainId, payTo), the funding wallet's EIP-2612
** permit and x402 exact's EIP-3009 TransferWithAuthorization, each digest computed by store.c's pure functions and signed
** with the vector's key: the same digest and the same 65 bytes (RFC 6979) as the Python reference and the operators'
** TypeScript. argv[1] is vectors.json.
*/
#include "test_lib.h"
#include "store.h"
#include "crypto.h"
#include "eth/cJSON.h"

static cJSON *g_vectors;
static HppliteCrypto *g_crypto;

static int hexTo(const char *hex, unsigned char *out, size_t n) {
    return hex && strlen(hex) == 2 + 2 * n && hpplite_hex_to_bytes(hex + 2, out, n) >= 0 ? 0 : -1;
}

/* a field as its EIP-712 word: an address right-aligned, a bytes32 as is, a uint256 that fits a JSON number */
static int wordOf(const char *zType, cJSON *f, unsigned char out[32]) {
    memset(out, 0, 32);
    if (strcmp(zType, "address") == 0) return cJSON_IsString(f) ? hexTo(f->valuestring, out + 12, 20) : -1;
    if (strcmp(zType, "bytes32") == 0) return cJSON_IsString(f) ? hexTo(f->valuestring, out, 32) : -1;
    if (strcmp(zType, "uint256") == 0 && cJSON_IsNumber(f)) {
        uint64_t v = (uint64_t)f->valuedouble;
        int i;
        for (i = 0; i < 8; i++) out[31 - i] = (unsigned char)(v >> (8 * i));
        return 0;
    }
    return -1;
}

/* the field types of "Name(type a,type b,...)" in order */
static int typesOf(const char *zTypeString, char types[8][16]) {
    const char *p = strchr(zTypeString, '(');
    int n = 0;
    while (p && *p && *p != ')' && n < 8) {
        size_t k = 0;
        p++;
        while (*p == ' ') p++;
        while (*p && *p != ' ' && k < 15) types[n][k++] = *p++;
        types[n++][k] = 0;
        while (*p && *p != ',' && *p != ')') p++;
    }
    return n;
}

/* sign with the vector's key and compare the 65 bytes */
static int signsAs(cJSON *m, const unsigned char digest[32]) {
    unsigned char priv[32], want[65], got[65], v;
    HppliteKeypair kp;
    HppliteSignature hs;
    cJSON *k = cJSON_GetObjectItem(m, "key"), *s = cJSON_GetObjectItem(m, "signature");
    if (!k || !s || hexTo(k->valuestring, priv, 32) || hexTo(s->valuestring, want, 65)) return 0;
    if (hpplite_crypto_keypair_from_privkey(g_crypto, &kp, priv) != 0 || hpplite_crypto_sign(g_crypto, &kp, digest, &hs) != 0) return 0;
    hpplite_sig_to_eth_format(&hs, &v, got, got + 32);
    got[64] = v;
    return memcmp(got, want, 65) == 0;
}

static int digestIs(cJSON *m, const unsigned char digest[32]) {
    unsigned char want[32];
    cJSON *d = cJSON_GetObjectItem(m, "digest");
    return d && hexTo(d->valuestring, want, 32) == 0 && memcmp(digest, want, 32) == 0;
}

static void test_store_messages(void) {
    TEST("every store message: hpplite_eip712_store_digest and the node's signer give the vectors' digest and signature");
    cJSON *sect = cJSON_GetObjectItem(g_vectors, "bucketMessages"), *m;
    unsigned char payTo[20], words[32 * 7], digest[32];
    uint64_t chainId;
    int seen = 0, i, binds = 0;
    if (!sect || hexTo(cJSON_GetObjectItem(sect, "payTo")->valuestring, payTo, 20)) FAIL("no bucketMessages in vectors.json");
    chainId = (uint64_t)cJSON_GetObjectItem(sect, "chainId")->valuedouble;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(sect, "messages")) {
        const char *ts = cJSON_GetObjectItem(m, "typeString")->valuestring;
        char types[8][16];
        int n = typesOf(ts, types);
        cJSON *fields = cJSON_GetObjectItem(m, "fields");
        if (n != cJSON_GetArraySize(fields)) FAILF("%s: %d types, %d fields", ts, n, cJSON_GetArraySize(fields));
        for (i = 0; i < n; i++)
            if (wordOf(types[i], cJSON_GetArrayItem(fields, i), words + 32 * i) != 0) FAILF("%s: field %d (%s) unreadable", ts, i, types[i]);
        hpplite_eip712_store_digest(chainId, payTo, ts, words, (size_t)n, digest);
        if (!digestIs(m, digest)) FAILF("%s: digest differs", ts);
        if (!signsAs(m, digest)) FAILF("%s: signature differs", ts);
        if (strncmp(ts, "Bind(", 5) == 0) binds++;
        seen++;
    }
    if (seen < 8 || binds != 1) FAILF("%d store messages, %d Bind: the vectors changed shape", seen, binds);
    PASS();
}

static void test_payment_messages(void) {
    TEST("the funding wallet's Permit and x402's TransferWithAuthorization: the node's digests and signatures are the vectors'");
    cJSON *sect = cJSON_GetObjectItem(g_vectors, "paymentMessages"), *m;
    uint64_t chainId;
    int permits = 0, transfers = 0;
    if (!sect) FAIL("no paymentMessages in vectors.json");
    chainId = (uint64_t)cJSON_GetObjectItem(sect, "chainId")->valuedouble;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(sect, "messages")) {
        cJSON *dom = cJSON_GetObjectItem(m, "domain"), *f = cJSON_GetObjectItem(m, "fields");
        const char *t = cJSON_GetObjectItem(m, "type")->valuestring;
        const char *name = cJSON_GetObjectItem(dom, "name")->valuestring, *version = cJSON_GetObjectItem(dom, "version")->valuestring;
        unsigned char asset[20], a[20], b[20], nonce[32], digest[32];
        if (hexTo(cJSON_GetObjectItem(dom, "verifyingContract")->valuestring, asset, 20)
            || hexTo(cJSON_GetArrayItem(f, 0)->valuestring, a, 20) || hexTo(cJSON_GetArrayItem(f, 1)->valuestring, b, 20)) FAILF("%s: unreadable", t);
        uint64_t x2 = (uint64_t)cJSON_GetArrayItem(f, 2)->valuedouble, x3 = (uint64_t)cJSON_GetArrayItem(f, 3)->valuedouble,
                 x4 = (uint64_t)cJSON_GetArrayItem(f, 4)->valuedouble;
        if (strcmp(t, "Permit") == 0) {
            hpplite_eip712_permit_digest(name, version, chainId, asset, a, b, x2, x3, x4, digest);
            permits++;
        } else if (strcmp(t, "TransferWithAuthorization") == 0) {
            if (hexTo(cJSON_GetArrayItem(f, 5)->valuestring, nonce, 32)) FAILF("%s: nonce unreadable", t);
            hpplite_eip712_transfer_digest(name, version, chainId, asset, a, b, x2, x3, x4, nonce, digest);
            transfers++;
        } else FAILF("an unknown payment message: %s", t);
        if (!digestIs(m, digest)) FAILF("%s (%s): digest differs", t, name);
        if (!signsAs(m, digest)) FAILF("%s (%s): signature differs", t, name);
    }
    if (permits < 2 || transfers < 2) FAILF("%d permits, %d transfers: the vectors changed shape", permits, transfers);
    PASS();
}

/* a decimal past 2^64 as its word (#358: token amounts with 18 decimals, Permit2's nonces) */
static int decWord(const char *z, unsigned char out[32]) {
    unsigned __int128 v = 0;
    int i;
    if (!z || !*z) return -1;
    for (; *z; z++) { if (*z < '0' || *z > '9') return -1; v = v * 10 + (unsigned)(*z - '0'); }
    memset(out, 0, 32);
    for (i = 0; i < 16; i++) out[31 - i] = (unsigned char)(v >> (8 * i));
    return 0;
}

static void test_permit2_messages(void) {
    TEST("x402 upto's Permit2 PermitWitnessTransferFrom (#358), amounts past 2^64: the digests and signatures of vectors.json");
    cJSON *sect = cJSON_GetObjectItem(g_vectors, "permit2Messages"), *m;
    uint64_t chainId;
    int n = 0;
    if (!sect) FAIL("no permit2Messages in vectors.json");
    chainId = (uint64_t)cJSON_GetObjectItem(sect, "chainId")->valuedouble;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(sect, "messages")) {
        cJSON *a = cJSON_GetObjectItem(m, "authorization"), *p = cJSON_GetObjectItem(a, "permitted"), *w = cJSON_GetObjectItem(a, "witness");
        unsigned char token[20], to[20], fac[20], amountW[32], nonceW[32], digest[32];
        if (!a || !p || !w || hexTo(cJSON_GetObjectItem(p, "token")->valuestring, token, 20) || hexTo(cJSON_GetObjectItem(w, "to")->valuestring, to, 20)
            || hexTo(cJSON_GetObjectItem(w, "facilitator")->valuestring, fac, 20) || decWord(cJSON_GetObjectItem(p, "amount")->valuestring, amountW)
            || decWord(cJSON_GetObjectItem(a, "nonce")->valuestring, nonceW)) FAIL("a Permit2 message: unreadable");
        hpplite_eip712_permit2_upto_digest(chainId, token, amountW, nonceW, strtoull(cJSON_GetObjectItem(a, "deadline")->valuestring, NULL, 10), to, fac,
                                           strtoull(cJSON_GetObjectItem(w, "validAfter")->valuestring, NULL, 10), digest);
        if (!digestIs(m, digest)) FAILF("Permit2 message %d: digest differs", n);
        if (!signsAs(m, digest)) FAILF("Permit2 message %d: signature differs", n);
        n++;
    }
    if (n < 2) FAILF("%d Permit2 messages: the vectors changed shape", n);
    PASS();
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "operators/conformance/vectors.json";
    FILE *f = fopen(path, "rb");
    char *text;
    long n;
    printf("=== Signing vectors (#342) ===\n");
    test_suite_init("signing_vectors", NULL, NULL);
    if (!f) { printf("cannot open %s\n", path); return 1; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    text = malloc((size_t)n + 1);
    if (!text || fread(text, 1, (size_t)n, f) != (size_t)n) { fclose(f); return 1; }
    text[n] = 0;
    fclose(f);
    g_vectors = cJSON_Parse(text);
    free(text);
    g_crypto = hpplite_crypto_init();
    if (!g_vectors || !g_crypto) { printf("vectors.json unreadable, or no crypto\n"); return 1; }
    test_store_messages();
    test_payment_messages();
    test_permit2_messages();
    cJSON_Delete(g_vectors);
    hpplite_crypto_free(g_crypto);
    return test_report();
}
