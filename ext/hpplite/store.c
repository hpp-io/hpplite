/*
** HPPLite stores (#11): the pin-service store over HTTP.
**
** put: POST <url>/put with the bytes; the provider answers {cid, receipt,
** ...} or 402 with PAYMENT-REQUIRED (x402). A 402 is paid once: the exact
** scheme's EIP-3009 TransferWithAuthorization signed with the node's key
** for the quoted amount, sent back in PAYMENT-SIGNATURE; above storecap=
** the call is refused before anything is signed.
** get: GET <url>/get/<ref>: the bytes.
*/
#include "store.h"
#include "hpplite_sqlite.h"
#include "crypto.h"
#include "cJSON.h"
#include "eth_client.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

struct HppliteStore {
    char *url;                      /* no trailing slash */
    int hasKey;
    HppliteKeypair kp;
    unsigned char address[20];
    uint64_t cap;
    uint64_t validFor;              /* seconds an authorization stays valid (a test chain whose clock ran ahead needs more) */
    uint64_t spent;
    int calls;
    int lastFailure;                /* HPPLITE_STORE_FAIL_* */
    int hasSigner;                  /* the provider's /status named its receipt signer and domain */
    unsigned char signer[20];
    unsigned char payTo[20];        /* the receipt domain's verifyingContract */
    uint64_t chainId;
    char error[256];
    HppliteCrypto *crypto;
};

typedef struct Buf { char *p; size_t n; } Buf;

static size_t bufWrite(void *data, size_t size, size_t nmemb, void *arg) {
    Buf *b = (Buf *)arg;
    size_t n = size * nmemb;
    char *q = realloc(b->p, b->n + n + 1);
    if (!q) return 0;
    memcpy(q + b->n, data, n);
    b->n += n;
    q[b->n] = 0;
    b->p = q;
    return n;
}

/* One request; the body, the status and the one response header we care about */
static long storeRequest(HppliteStore *s, const char *method, const char *path, const uint8_t *body, size_t bodyLen,
                         const char *extraHeader, Buf *out, char **pzPaymentRequired, char **pzPaymentResponse) {
    CURL *c = curl_easy_init();
    struct curl_slist *hdrs = NULL;
    Buf headers = {0};
    char url[1200];
    long status = 0;
    if (!c) return 0;
    snprintf(url, sizeof url, "%s%s", s->url, path);
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, bufWrite);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, bufWrite);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &headers);
    if (body) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long)bodyLen);
        hdrs = curl_slist_append(hdrs, "Content-Type: application/octet-stream");
    } else if (strcmp(method, "POST") == 0) {
        curl_easy_setopt(c, CURLOPT_POST, 1L);
        curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, 0L);
    }
    if (extraHeader) hdrs = curl_slist_append(hdrs, extraHeader);
    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    if (curl_easy_perform(c) != CURLE_OK) {
        snprintf(s->error, sizeof s->error, "store unreachable: %s", url);
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    }
    /* the x402 headers, case-insensitively */
    if (headers.p) {
        char *line = headers.p, *next;
        for (; line && *line; line = next) {
            size_t len;
            next = strchr(line, '\n');
            if (next) next++;
            len = next ? (size_t)(next - line) : strlen(line);
            while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
            if (pzPaymentRequired && len > 17 && strncasecmp(line, "PAYMENT-REQUIRED:", 17) == 0) {
                const char *v = line + 17;
                while (*v == ' ') v++;
                *pzPaymentRequired = sqlite3_mprintf("%.*s", (int)(len - (v - line)), v);
            } else if (pzPaymentResponse && len > 17 && strncasecmp(line, "PAYMENT-RESPONSE:", 17) == 0) {
                const char *v = line + 17;
                while (*v == ' ') v++;
                *pzPaymentResponse = sqlite3_mprintf("%.*s", (int)(len - (v - line)), v);
            }
        }
        free(headers.p);
    }
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(c);
    return status;
}

/* === base64 (the x402 headers) === */

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64encode(const unsigned char *in, size_t n) {
    char *out = sqlite3_malloc((int)((n + 2) / 3 * 4 + 1)), *p = out;
    size_t i;
    if (!out) return NULL;
    for (i = 0; i + 2 < n; i += 3) {
        *p++ = b64chars[in[i] >> 2];
        *p++ = b64chars[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        *p++ = b64chars[((in[i + 1] & 15) << 2) | (in[i + 2] >> 6)];
        *p++ = b64chars[in[i + 2] & 63];
    }
    if (i < n) {
        *p++ = b64chars[in[i] >> 2];
        if (i + 1 < n) {
            *p++ = b64chars[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            *p++ = b64chars[(in[i + 1] & 15) << 2];
        } else {
            *p++ = b64chars[(in[i] & 3) << 4];
            *p++ = '=';
        }
        *p++ = '=';
    }
    *p = 0;
    return out;
}

static unsigned char *b64decode(const char *in, size_t *pLen) {
    size_t n = strlen(in), i, o = 0;
    unsigned char *out = malloc(n / 4 * 3 + 4);
    unsigned v = 0;
    int bits = 0;
    if (!out) return NULL;
    for (i = 0; i < n; i++) {
        const char *q = strchr(b64chars, in[i]);
        if (in[i] == '=' ) break;
        if (!q || !in[i]) continue;
        v = (v << 6) | (unsigned)(q - b64chars);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char)((v >> bits) & 0xff);
        }
    }
    out[o] = 0;
    *pLen = o;
    return out;
}

/* === x402: the exact scheme, EIP-3009 === */

static void wordInt(uint64_t v, unsigned char out[32]) {
    int i;
    memset(out, 0, 32);
    for (i = 0; i < 8; i++) out[31 - i] = (unsigned char)(v >> (8 * i));
}

static int wordAddress(const char *hex, unsigned char out[32]) {
    memset(out, 0, 32);
    return (hex && strlen(hex) == 42 && hpplite_hex_to_bytes(hex + 2, out + 12, 20) >= 0) ? 0 : -1;
}

/* A payment for one 402: the PAYMENT-SIGNATURE value (sqlite3_mprintf), or NULL with the reason in s->error */
static char *storePay(HppliteStore *s, const char *zRequired, uint64_t *pAmount) {
    size_t n = 0;
    unsigned char *json = b64decode(zRequired, &n);
    cJSON *req = json ? cJSON_Parse((const char *)json) : NULL;
    cJSON *accepts = req ? cJSON_GetObjectItem(req, "accepts") : NULL, *acc = NULL, *it;
    char *result = NULL;
    free(json);
    if (!req) { snprintf(s->error, sizeof s->error, "402 without a readable PAYMENT-REQUIRED"); return NULL; }
    cJSON_ArrayForEach(it, accepts) {
        cJSON *scheme = cJSON_GetObjectItem(it, "scheme");
        if (scheme && cJSON_IsString(scheme) && strcmp(scheme->valuestring, "exact") == 0) { acc = it; break; }
    }
    if (!acc) { snprintf(s->error, sizeof s->error, "the store offers no exact-scheme payment"); cJSON_Delete(req); return NULL; }
    {
        cJSON *amount = cJSON_GetObjectItem(acc, "amount"), *asset = cJSON_GetObjectItem(acc, "asset"),
              *payTo = cJSON_GetObjectItem(acc, "payTo"), *network = cJSON_GetObjectItem(acc, "network"),
              *extra = cJSON_GetObjectItem(acc, "extra");
        cJSON *name = extra ? cJSON_GetObjectItem(extra, "name") : NULL, *version = extra ? cJSON_GetObjectItem(extra, "version") : NULL;
        uint64_t amt, chainId = 0;
        if (!amount || !cJSON_IsString(amount) || !asset || !cJSON_IsString(asset) || !payTo || !cJSON_IsString(payTo)
            || !network || !cJSON_IsString(network) || !name || !cJSON_IsString(name) || !version || !cJSON_IsString(version)) {
            snprintf(s->error, sizeof s->error, "the 402's offer is incomplete");
            cJSON_Delete(req);
            return NULL;
        }
        amt = strtoull(amount->valuestring, NULL, 10);
        if (!s->hasKey) { snprintf(s->error, sizeof s->error, "the store asks %llu and this node has no key to pay with", (unsigned long long)amt); cJSON_Delete(req); return NULL; }
        if (amt > s->cap) { snprintf(s->error, sizeof s->error, "the store asks %llu, storecap is %llu: refused", (unsigned long long)amt, (unsigned long long)s->cap); cJSON_Delete(req); return NULL; }
        if (strncmp(network->valuestring, "eip155:", 7) == 0) chainId = strtoull(network->valuestring + 7, NULL, 10);

        /* domain = keccak(typehash, keccak(name), keccak(version), chainId, asset) */
        unsigned char domain[32], w[32 * 7], h[32], digest[32], nonce[32], sig[65], t[32];
        static const char zDomainType[] = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
        static const char zTransferType[] = "TransferWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)";
        hpplite_keccak256((const unsigned char *)zDomainType, sizeof(zDomainType) - 1, w);
        hpplite_keccak256((const unsigned char *)name->valuestring, strlen(name->valuestring), w + 32);
        hpplite_keccak256((const unsigned char *)version->valuestring, strlen(version->valuestring), w + 64);
        wordInt(chainId, w + 96);
        if (wordAddress(asset->valuestring, w + 128) != 0) { snprintf(s->error, sizeof s->error, "bad asset address in the 402"); cJSON_Delete(req); return NULL; }
        hpplite_keccak256(w, 160, domain);

        uint64_t now = (uint64_t)time(NULL), validAfter = now - 60, validBefore = now + s->validFor;
        {
            /* a nonce used once: time, the address, a counter and this store's identity, hashed (uniqueness, not secrecy) */
            unsigned char seed[84];
            static unsigned counter;
            wordInt(now, seed);
            memcpy(seed + 32, s->address, 20);
            wordInt(++counter + (uint64_t)(uintptr_t)s, seed + 52);
            hpplite_keccak256(seed, sizeof seed, nonce);
        }
        hpplite_keccak256((const unsigned char *)zTransferType, sizeof(zTransferType) - 1, w);
        memset(w + 32, 0, 32); memcpy(w + 32 + 12, s->address, 20);
        if (wordAddress(payTo->valuestring, w + 64) != 0) { snprintf(s->error, sizeof s->error, "bad payTo in the 402"); cJSON_Delete(req); return NULL; }
        wordInt(amt, w + 96);
        wordInt(validAfter, w + 128);
        wordInt(validBefore, w + 160);
        memcpy(w + 192, nonce, 32);
        hpplite_keccak256(w, 224, h);
        (void)t;
        {
            unsigned char pre[66];
            pre[0] = 0x19; pre[1] = 0x01;
            memcpy(pre + 2, domain, 32);
            memcpy(pre + 34, h, 32);
            hpplite_keccak256(pre, 66, digest);
        }
        {
            HppliteSignature hs;
            unsigned char v;
            if (hpplite_crypto_sign(s->crypto, &s->kp, digest, &hs) != 0) { snprintf(s->error, sizeof s->error, "could not sign the payment"); cJSON_Delete(req); return NULL; }
            hpplite_sig_to_eth_format(&hs, &v, sig, sig + 32);   /* r, s, then v: the 65-byte form ecrecover expects */
            sig[64] = v;
        }
        /* the payload, as the spec lays it out */
        {
            char fromHex[43], nonceHex[67], sigHex[133], *payloadJson, *b64;
            cJSON *payload = cJSON_CreateObject(), *inner = cJSON_CreateObject(), *auth = cJSON_CreateObject();
            char num[32];
            snprintf(fromHex, sizeof fromHex, "0x");
            hpplite_bytes_to_hex(s->address, 20, fromHex + 2);
            snprintf(nonceHex, sizeof nonceHex, "0x");
            hpplite_bytes_to_hex(nonce, 32, nonceHex + 2);
            snprintf(sigHex, sizeof sigHex, "0x");
            hpplite_bytes_to_hex(sig, 65, sigHex + 2);
            cJSON_AddNumberToObject(payload, "x402Version", 2);
            if (cJSON_GetObjectItem(req, "resource")) cJSON_AddItemToObject(payload, "resource", cJSON_Duplicate(cJSON_GetObjectItem(req, "resource"), 1));
            cJSON_AddItemToObject(payload, "accepted", cJSON_Duplicate(acc, 1));
            cJSON_AddStringToObject(auth, "from", fromHex);
            cJSON_AddStringToObject(auth, "to", payTo->valuestring);
            cJSON_AddStringToObject(auth, "value", amount->valuestring);
            snprintf(num, sizeof num, "%llu", (unsigned long long)validAfter);
            cJSON_AddStringToObject(auth, "validAfter", num);
            snprintf(num, sizeof num, "%llu", (unsigned long long)validBefore);
            cJSON_AddStringToObject(auth, "validBefore", num);
            cJSON_AddStringToObject(auth, "nonce", nonceHex);
            cJSON_AddStringToObject(inner, "signature", sigHex);
            cJSON_AddItemToObject(inner, "authorization", auth);
            cJSON_AddItemToObject(payload, "payload", inner);
            cJSON_AddItemToObject(payload, "extensions", cJSON_CreateObject());
            payloadJson = cJSON_PrintUnformatted(payload);
            cJSON_Delete(payload);
            b64 = payloadJson ? b64encode((const unsigned char *)payloadJson, strlen(payloadJson)) : NULL;
            free(payloadJson);
            if (b64) { result = sqlite3_mprintf("PAYMENT-SIGNATURE: %s", b64); sqlite3_free(b64); }
        }
        if (pAmount) *pAmount = amt;
    }
    cJSON_Delete(req);
    return result;
}

/* === The API === */

HppliteStore *hpplite_store_open(const char *url, const uint8_t *privkey, uint64_t cap) {
    HppliteStore *s;
    size_t n;
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) return NULL;
    s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->url = strdup(url);
    n = strlen(s->url);
    while (n > 0 && s->url[n - 1] == '/') s->url[--n] = 0;
    s->cap = cap;
    s->validFor = 120;
    {
        /* tests on a chain whose clock ran ahead (anvil after evm_increaseTime) widen the window */
        const char *z = getenv("HPPLITE_STORE_VALID_FOR");
        if (z && atoll(z) > 0) s->validFor = (uint64_t)atoll(z);
    }
    s->crypto = hpplite_crypto_init();
    if (privkey && s->crypto) {
        if (hpplite_crypto_keypair_from_privkey(s->crypto, &s->kp, privkey) == 0) {
            memcpy(s->address, s->kp.address, 20);
            s->hasKey = 1;
        }
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    {
        /* the provider's /status: which key signs its receipts, in which domain */
        Buf out = {0};
        if (storeRequest(s, "GET", "/status", NULL, 0, NULL, &out, NULL, NULL) == 200 && out.p) {
            cJSON *st = cJSON_Parse(out.p);
            cJSON *signer = st ? cJSON_GetObjectItem(st, "signer") : NULL;
            cJSON *receipt = st ? cJSON_GetObjectItem(st, "receipt") : NULL;
            cJSON *domain = receipt ? cJSON_GetObjectItem(receipt, "domain") : NULL;
            cJSON *chainId = domain ? cJSON_GetObjectItem(domain, "chainId") : NULL;
            cJSON *contract = domain ? cJSON_GetObjectItem(domain, "verifyingContract") : NULL;
            if (signer && cJSON_IsString(signer) && strlen(signer->valuestring) == 42
                && contract && cJSON_IsString(contract) && strlen(contract->valuestring) == 42 && chainId
                && hpplite_hex_to_bytes(signer->valuestring + 2, s->signer, 20) >= 0
                && hpplite_hex_to_bytes(contract->valuestring + 2, s->payTo, 20) >= 0) {
                s->chainId = cJSON_IsNumber(chainId) ? (uint64_t)chainId->valuedouble : strtoull(chainId->valuestring ? chainId->valuestring : "0", NULL, 10);
                s->hasSigner = 1;
            }
            cJSON_Delete(st);
        }
        free(out.p);
        s->error[0] = 0;
    }
    return s;
}

int hpplite_store_signer(HppliteStore *s, unsigned char out[20]) {
    if (!s || !s->hasSigner) return -1;
    memcpy(out, s->signer, 20);
    return 0;
}

int hpplite_store_domain(HppliteStore *s, unsigned char payTo[20], uint64_t *pChainId) {
    if (!s || !s->hasSigner) return -1;
    if (payTo) memcpy(payTo, s->payTo, 20);
    if (pChainId) *pChainId = s->chainId;
    return 0;
}

int hpplite_store_last_failure(HppliteStore *s) { return s ? s->lastFailure : HPPLITE_STORE_FAIL_OTHER; }

void hpplite_store_close(HppliteStore *s) {
    if (!s) return;
    if (s->crypto) hpplite_crypto_free(s->crypto);
    memset(&s->kp, 0, sizeof s->kp);
    free(s->url);
    free(s);
}

void hpplite_store_set_valid_for(HppliteStore *s, uint64_t seconds) { if (s && seconds) s->validFor = seconds; }

int hpplite_store_receipt_compact(const char *zJson, const unsigned char payTo[20], uint8_t out[HPPLITE_RECEIPT_SIZE]) {
    cJSON *r = zJson ? cJSON_Parse(zJson) : NULL;
    cJSON *signer = r ? cJSON_GetObjectItem(r, "signer") : NULL, *expires = r ? cJSON_GetObjectItem(r, "expires") : NULL,
          *sig = r ? cJSON_GetObjectItem(r, "signature") : NULL;
    uint64_t exp;
    int i;
    if (!signer || !cJSON_IsString(signer) || strlen(signer->valuestring) != 42
        || !sig || !cJSON_IsString(sig) || strlen(sig->valuestring) != 132 || !expires) { cJSON_Delete(r); return -1; }
    exp = cJSON_IsNumber(expires) ? (uint64_t)expires->valuedouble : (uint64_t)strtoull(expires->valuestring ? expires->valuestring : "0", NULL, 10);
    out[0] = 1;
    if (hpplite_hex_to_bytes(signer->valuestring + 2, out + 1, 20) < 0 || hpplite_hex_to_bytes(sig->valuestring + 2, out + 49, 65) < 0) { cJSON_Delete(r); return -1; }
    memcpy(out + 21, payTo, 20);
    for (i = 0; i < 8; i++) out[41 + 7 - i] = (uint8_t)(exp >> (8 * i));
    cJSON_Delete(r);
    return HPPLITE_RECEIPT_SIZE;
}

int hpplite_store_receipt_decode(const uint8_t *receipt, size_t n, unsigned char signer[20], unsigned char payTo[20],
                                 uint64_t *pExpires, unsigned char sig[65]) {
    uint64_t exp = 0;
    int i;
    if (!receipt || n != HPPLITE_RECEIPT_SIZE || receipt[0] != 1) return -1;
    if (signer) memcpy(signer, receipt + 1, 20);
    if (payTo) memcpy(payTo, receipt + 21, 20);
    for (i = 0; i < 8; i++) exp = (exp << 8) | receipt[41 + i];
    if (pExpires) *pExpires = exp;
    if (sig) memcpy(sig, receipt + 49, 65);
    return 0;
}

const char *hpplite_store_error(HppliteStore *s) { return s ? s->error : "no store"; }
const char *hpplite_store_url(HppliteStore *s) { return s ? s->url : NULL; }
uint64_t hpplite_store_spent(HppliteStore *s, int *pCalls) { if (pCalls) *pCalls = s ? s->calls : 0; return s ? s->spent : 0; }

char *hpplite_store_put(HppliteStore *s, const uint8_t *data, size_t len, char **pzReceipt) {
    Buf out = {0};
    char *zRequired = NULL, *zResponse = NULL, *zRef = NULL;
    long status;
    if (pzReceipt) *pzReceipt = NULL;
    if (!s || !data || !len) return NULL;
    s->error[0] = 0;
    s->lastFailure = HPPLITE_STORE_FAIL_NONE;
    status = storeRequest(s, "POST", "/put", data, len, NULL, &out, &zRequired, NULL);
    if (status == 402 && zRequired) {
        uint64_t amount = 0;
        char *zPay = storePay(s, zRequired, &amount);
        free(out.p); out.p = NULL; out.n = 0;
        if (!zPay) { s->lastFailure = strstr(s->error, "storecap") ? HPPLITE_STORE_FAIL_CAP : HPPLITE_STORE_FAIL_OTHER; sqlite3_free(zRequired); return NULL; }
        status = storeRequest(s, "POST", "/put", data, len, zPay, &out, NULL, &zResponse);
        sqlite3_free(zPay);
        if (status == 200) { s->spent += amount; s->calls++; }
    }
    sqlite3_free(zRequired);
    sqlite3_free(zResponse);
    if (status != 200) {
        if (!s->error[0]) snprintf(s->error, sizeof s->error, "store answered %ld to put: %.120s", status, out.p ? out.p : "");
        s->lastFailure = status == 0 ? HPPLITE_STORE_FAIL_UNREACHABLE : HPPLITE_STORE_FAIL_OTHER;
        free(out.p);
        return NULL;
    }
    {
        cJSON *r = out.p ? cJSON_Parse(out.p) : NULL;
        cJSON *cid = r ? cJSON_GetObjectItem(r, "cid") : NULL, *receipt = r ? cJSON_GetObjectItem(r, "receipt") : NULL;
        if (cid && cJSON_IsString(cid) && cid->valuestring[0]) {
            zRef = sqlite3_mprintf("%s", cid->valuestring);
            if (receipt && cJSON_IsObject(receipt)) {
                char *z = cJSON_PrintUnformatted(receipt);
                /* the receipt must be the provider's own promise over these bytes, or it is worth nothing */
                if (z && s->hasSigner) {
                    uint8_t compact[HPPLITE_RECEIPT_SIZE];
                    unsigned char h[32];
                    hpplite_keccak256(data, len, h);
                    if (hpplite_store_receipt_compact(z, s->payTo, compact) != HPPLITE_RECEIPT_SIZE
                        || hpplite_store_receipt_verify(compact, HPPLITE_RECEIPT_SIZE, h, zRef, s->chainId, s->signer) != 0) {
                        snprintf(s->error, sizeof s->error, "the store's receipt for %s does not verify against its published signer", zRef);
                        s->lastFailure = HPPLITE_STORE_FAIL_RECEIPT;
                        sqlite3_free(zRef);
                        zRef = NULL;
                    }
                }
                if (zRef && pzReceipt && z) *pzReceipt = sqlite3_mprintf("%s", z);
                free(z);
            }
        } else {
            snprintf(s->error, sizeof s->error, "store's answer to put names no cid");
            s->lastFailure = HPPLITE_STORE_FAIL_OTHER;
        }
        cJSON_Delete(r);
    }
    free(out.p);
    return zRef;
}

/* The receipt's domain ("HPPLitePinService", "1", chainId, payTo) and struct Receipt(bytes32 hash, string cid, uint256 expires) */
static void receiptDigest(const unsigned char hash[32], const char *zCid, uint64_t expires, uint64_t chainId,
                          const unsigned char payTo[20], unsigned char digest[32]) {
    static const char zDomainType[] = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    static const char zReceiptType[] = "Receipt(bytes32 hash,string cid,uint256 expires)";
    unsigned char w[160], domain[32], st[32], pre[66];
    hpplite_keccak256((const unsigned char *)zDomainType, sizeof(zDomainType) - 1, w);
    hpplite_keccak256((const unsigned char *)"HPPLitePinService", 17, w + 32);
    hpplite_keccak256((const unsigned char *)"1", 1, w + 64);
    wordInt(chainId, w + 96);
    memset(w + 128, 0, 32); memcpy(w + 128 + 12, payTo, 20);
    hpplite_keccak256(w, 160, domain);
    hpplite_keccak256((const unsigned char *)zReceiptType, sizeof(zReceiptType) - 1, w);
    memcpy(w + 32, hash, 32);
    hpplite_keccak256((const unsigned char *)zCid, strlen(zCid), w + 64);
    wordInt(expires, w + 96);
    hpplite_keccak256(w, 128, st);
    pre[0] = 0x19; pre[1] = 0x01;
    memcpy(pre + 2, domain, 32);
    memcpy(pre + 34, st, 32);
    hpplite_keccak256(pre, 66, digest);
}

int hpplite_store_receipt_verify(const uint8_t *receipt, size_t n, const unsigned char hash[32], const char *zCid,
                                 uint64_t chainId, const unsigned char *expectedSigner) {
    unsigned char signer[20], payTo[20], sig[65], digest[32], pub[33], addr[20];
    uint64_t expires = 0;
    HppliteSignature hs;
    HppliteCrypto *crypto;
    int rc;
    if (hpplite_store_receipt_decode(receipt, n, signer, payTo, &expires, sig) != 0 || !hash || !zCid) return -1;
    if (expectedSigner && memcmp(expectedSigner, signer, 20) != 0) return -1;
    receiptDigest(hash, zCid, expires, chainId, payTo, digest);
    memcpy(hs.sig, sig, 64);
    hs.recid = sig[64] >= 27 ? sig[64] - 27 : sig[64];
    if (hs.recid > 1) return -1;
    crypto = hpplite_crypto_init();
    if (!crypto) return -1;
    rc = hpplite_crypto_recover_pubkey(crypto, digest, &hs, pub);
    hpplite_crypto_free(crypto);
    if (rc != 0 || eth_address_from_compressed_pubkey(pub, addr) != 0) return -1;
    return memcmp(addr, signer, 20) == 0 ? 0 : -1;
}

uint8_t *hpplite_store_get(HppliteStore *s, const char *zRef, size_t *pLen) {
    Buf out = {0};
    char path[600];
    long status;
    if (pLen) *pLen = 0;
    if (!s || !zRef || !zRef[0] || strlen(zRef) > 500 || strchr(zRef, '/')) return NULL;
    s->error[0] = 0;
    snprintf(path, sizeof path, "/get/%s", zRef);
    status = storeRequest(s, "GET", path, NULL, 0, NULL, &out, NULL, NULL);
    if (status != 200) {
        if (!s->error[0]) snprintf(s->error, sizeof s->error, "store answered %ld to get %s", status, zRef);
        free(out.p);
        return NULL;
    }
    if (!out.p) { out.p = malloc(1); if (out.p) out.p[0] = 0; }
    if (pLen) *pLen = out.n;
    return (uint8_t *)out.p;                      /* malloc'd, terminated */
}
