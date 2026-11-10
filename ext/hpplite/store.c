/*
** HPPLite stores (#11): a hpplite-store/2 store over HTTP (#214, doc/store-protocol.md).
**
** The rollup's bucket: bound at the first write with the node's key (the rollup's sequencer) and a
** funding key derived from it, given an EIP-2612 permit (HPPLITE_STORE_PERMIT) the store pulls what it is owed
** with. put: POST <url>/put?bucket=<da>&height=<n> with the bytes and the sequencer's Content
** signature; names: POST /name/<da>-<height>. A 402 (a write the account cannot cover) is paid once
** from the funding wallet in x402's exact scheme, an EIP-3009 TransferWithAuthorization for the quoted
** top-up, sent back in PAYMENT-SIGNATURE; above HPPLITE_STORE_CAP the call is refused before anything is
** signed. The relay is paid the same way from the node's own key, per call (x402's batch-settlement
** channels were removed in v0.8, #251).
** get: GET <url>/get/<ref>: the bytes.
*/
#include "store.h"
#include "hpplite_sqlite.h"
#include "crypto.h"
#include "cJSON.h"
#include "eth_client.h"
#include "sha256.h"
#include "spend.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <math.h>


struct HppliteStore {
    char *url;                      /* as given, no trailing slash: what the status row shows */
    char *base;                     /* http(s)://host:port the requests go to */
    int hasKey;
    HppliteKeypair kp;
    unsigned char address[20];
    uint64_t cap;
    uint64_t validFor;              /* seconds an authorization stays valid (a test chain whose clock ran ahead needs more) */
    uint64_t spent;
    int calls;                      /* ...paid calls */
    int nOk;                        /* puts and gets that succeeded */
    size_t lastBytes;               /* the last put's size: what a quote is for */
    int hasPayment;                 /* the provider's /status named its asset and prices (#11) */
    unsigned char asset[20];
    uint64_t payChainId;
    uint64_t pricePerByte, pricePerPin;
    double pricePerMiBMonth;        /* per MiB per 30 days, fractions allowed (#172) */
    uint64_t retentionSeconds;      /* what a put buys */
    int lastFailure;                /* HPPLITE_STORE_FAIL_* */
    long lastStatus;                /* the HTTP status of the last request, -1 none since taken (#229) */
    int hasSigner;                  /* the provider's /status named its signer and domain */
    unsigned char signer[20];
    unsigned char payTo[20];        /* the receipt domain's verifyingContract */
    uint64_t chainId;
    int isRelay;                    /* opened for relay= (#126): its messages name the relay and relaycap */
    char protocol[48];              /* what /status says it speaks ("hpplite-store/1"; empty: a service from before #188) */
    HppliteSpend *spend;            /* the node's ledger (#173): HPPLITE_SPEND_CAP checked before signing, charges recorded; not owned */
    /* hpplite-store/2 (#214, doc/store-protocol.md): the rollup's bucket, paid from a funding wallet */
    int v2;                         /* /status says hpplite-store/2 */
    int hasBucket;                  /* bucketDa below is set (hpplite_store_set_bucket) */
    unsigned char bucketDa[20];
    int bound;                      /* the store has the bucket bound to this node's funding wallet */
    time_t boundChecked;            /* when that was last asked */
    int hasFund;                    /* the funding key: derived from the node's key */
    int external;                   /* funder= (#272): fundAddr is another's wallet; no key, no binding, no permit from here */
    int funderWait;                 /* the last refusal is the external funder's to lift */
    HppliteKeypair fundKp;
    unsigned char fundAddr[20];
    uint64_t permitCap;             /* HPPLITE_STORE_PERMIT: what the permit lets the store pull (0: none, the 402s are paid) */
    time_t permitChecked;
    unsigned char spender[20];      /* who the permit names (/status buckets.spender) */
    char assetName[64], assetVersion[16];   /* the asset's EIP-712 domain, for the permit */
    uint64_t pricePerWrite;
    int64_t coveredUntil;           /* from the last statement or bucket read; -1: not known */
    char bucketState[16];
    char error[256];
    HppliteCrypto *crypto;
};

typedef struct Buf { char *p; size_t n; } Buf;

static void deriveFund(HppliteStore *s, const uint8_t privkey[32]);
static int ensureBucket(HppliteStore *s);
static void storeDigest(HppliteStore *s, const char *zType, const unsigned char *words, size_t nWords, unsigned char out[32]);
static int signWith(HppliteStore *s, HppliteKeypair *kp, const unsigned char digest[32], unsigned char sig[65]);
static void lowerHex(const unsigned char *b, size_t n, char *out);

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
    snprintf(url, sizeof url, "%s%s", s->base, path);
    curl_easy_setopt(c, CURLOPT_URL, url);
    if (getenv("HPPLITE_CA_BUNDLE")) curl_easy_setopt(c, CURLOPT_CAINFO, getenv("HPPLITE_CA_BUNDLE"));   /* the certificates to trust (#269) */
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
    if (extraHeader) {
        /* one header per line */
        const char *h = extraHeader, *nl;
        for (; *h; h = nl ? nl + 1 : h + strlen(h)) {
            char *line;
            nl = strchr(h, '\n');
            line = sqlite3_mprintf("%.*s", (int)(nl ? (size_t)(nl - h) : strlen(h)), h);   /* a payment header runs to kilobytes */
            if (line && line[0]) hdrs = curl_slist_append(hdrs, line);
            sqlite3_free(line);
            if (!nl) break;
        }
    }
    if (hdrs) curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdrs);
    if (curl_easy_perform(c) != CURLE_OK) {
        snprintf(s->error, sizeof s->error, "store unreachable: %s", url);
    } else {
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &status);
    }
    s->lastStatus = status;
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

/* EIP-712 (#11): what a payment signs */
static void typedHash(const unsigned char domain[32], const unsigned char structHash[32], unsigned char out[32]) {
    unsigned char pre[66];
    pre[0] = 0x19; pre[1] = 0x01;
    memcpy(pre + 2, domain, 32);
    memcpy(pre + 34, structHash, 32);
    hpplite_keccak256(pre, 66, out);
}

static void domainOf(const char *zName, const char *zVersion, uint64_t chainId, const unsigned char contract[32], unsigned char out[32]) {
    static const char zDomainType[] = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    unsigned char w[160];
    hpplite_keccak256((const unsigned char *)zDomainType, sizeof(zDomainType) - 1, w);
    hpplite_keccak256((const unsigned char *)zName, strlen(zName), w + 32);
    hpplite_keccak256((const unsigned char *)zVersion, strlen(zVersion), w + 64);
    wordInt(chainId, w + 96);
    memcpy(w + 128, contract, 32);
    hpplite_keccak256(w, 160, out);
}

static const char *jstr(cJSON *o, const char *k) {
    cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return v && cJSON_IsString(v) ? v->valuestring : NULL;
}

static uint64_t jnum(cJSON *o, const char *k) {
    cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    if (v && cJSON_IsString(v)) return strtoull(v->valuestring, NULL, 10);
    if (v && cJSON_IsNumber(v)) return (uint64_t)v->valuedouble;
    return 0;
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
        if (scheme && cJSON_IsString(scheme) && strcmp(scheme->valuestring, "exact") == 0 && !acc) acc = it;
    }
    if (acc && s->external) {
        /* funder= (#272): the account is the platform's; a 402 means it is not covered, and this node pays nothing */
        char fund[43];
        const char *za = jstr(acc, "amount");
        lowerHex(s->fundAddr, 20, fund);
        snprintf(s->error, sizeof s->error, "waiting for the funder %s to cover the bucket: the %s asks %s (the node pays nothing with funder=)", fund,
                 s->isRelay ? "relay" : "store", za ? za : "a top-up");
        s->funderWait = 1;
        cJSON_Delete(req);
        return NULL;
    }
    if (acc && s->spend) {
        /* HPPLITE_SPEND_CAP (#173): over the window's cap, nothing is signed; the batch waits as over HPPLITE_STORE_CAP */
        uint64_t asked = strtoull(jstr(acc, "amount") ? jstr(acc, "amount") : "0", NULL, 10);
        char why[200];
        if (hpplite_spend_allow(s->spend, asked, why, sizeof why) != 0) {
            snprintf(s->error, sizeof s->error, "the %s asks %llu: %s; refused", s->isRelay ? "relay" : "store", (unsigned long long)asked, why);
            cJSON_Delete(req);
            return NULL;
        }
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
        HppliteKeypair *payKp = s->v2 && s->hasFund ? &s->fundKp : &s->kp;          /* /2: the funding wallet pays (#214) */
        const unsigned char *payAddr = s->v2 && s->hasFund ? s->fundAddr : s->address;
        if (amt > s->cap) { snprintf(s->error, sizeof s->error, "the %s asks %llu, %s is %llu: refused", s->isRelay ? "relay" : "store",
                                     (unsigned long long)amt, s->isRelay ? "HPPLITE_RELAY_CAP" : "HPPLITE_STORE_CAP", (unsigned long long)s->cap); cJSON_Delete(req); return NULL; }
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
            memcpy(seed + 32, payAddr, 20);
            wordInt(++counter + (uint64_t)(uintptr_t)s, seed + 52);
            hpplite_keccak256(seed, sizeof seed, nonce);
        }
        hpplite_keccak256((const unsigned char *)zTransferType, sizeof(zTransferType) - 1, w);
        memset(w + 32, 0, 32); memcpy(w + 32 + 12, payAddr, 20);
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
            if (hpplite_crypto_sign(s->crypto, payKp, digest, &hs) != 0) { snprintf(s->error, sizeof s->error, "could not sign the payment"); cJSON_Delete(req); return NULL; }
            hpplite_sig_to_eth_format(&hs, &v, sig, sig + 32);   /* r, s, then v: the 65-byte form ecrecover expects */
            sig[64] = v;
        }
        /* the payload, as the spec lays it out */
        {
            char fromHex[43], nonceHex[67], sigHex[133], *payloadJson, *b64;
            cJSON *payload = cJSON_CreateObject(), *inner = cJSON_CreateObject(), *auth = cJSON_CreateObject();
            char num[32];
            snprintf(fromHex, sizeof fromHex, "0x");
            hpplite_bytes_to_hex(payAddr, 20, fromHex + 2);
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

/* CIDv1, raw (0x55), sha2-256 multihash, base32 lower with the 'b' prefix:
** what a store names bytes (and what Kubo with raw leaves named the same bytes) */
void hpplite_store_sha256(const uint8_t *data, size_t len, uint8_t out[32]) {
    SHA256_CTX c;
    hpplite_sha256_init(&c);
    hpplite_sha256_update(&c, data, len);
    hpplite_sha256_final(&c, out);
}

void hpplite_store_cid_from_sha256(const uint8_t digest[32], char out[64]) {
    static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz234567";
    uint8_t raw[36] = { 0x01, 0x55, 0x12, 0x20 };
    unsigned bits = 0, acc = 0;
    size_t i, n = 0;
    memcpy(raw + 4, digest, 32);
    out[n++] = 'b';
    for (i = 0; i < sizeof raw; i++) {
        acc = (acc << 8) | raw[i];
        bits += 8;
        while (bits >= 5) { out[n++] = alphabet[(acc >> (bits - 5)) & 31]; bits -= 5; }
    }
    if (bits) out[n++] = alphabet[(acc << (5 - bits)) & 31];
    out[n] = 0;
}

static void cidOf(const uint8_t *data, size_t len, char out[64]) {
    uint8_t digest[32];
    hpplite_store_sha256(data, len, digest);
    hpplite_store_cid_from_sha256(digest, out);
}

/* === The API === */

/* The provider's /status: its protocol, which key signs, in which domain, what it charges. The HTTP status */
static long statusRead(HppliteStore *s) {
    long code;
    /* the provider's /status: which key signs for it, in which domain */
    Buf out = {0};
    if ((code = storeRequest(s, "GET", "/status", NULL, 0, NULL, &out, NULL, NULL)) == 200 && out.p) {
        cJSON *st = cJSON_Parse(out.p);
        cJSON *signer = st ? cJSON_GetObjectItem(st, "signer") : NULL;
        cJSON *pay = st ? cJSON_GetObjectItem(st, "payment") : NULL;
        if (pay && cJSON_IsObject(pay)) {
            /* what it charges, for the status row: asset, chain, the price of a put */
            cJSON *asset = cJSON_GetObjectItem(pay, "asset"), *network = cJSON_GetObjectItem(pay, "network"),
                  *perByte = cJSON_GetObjectItem(pay, "pricePerByte"), *perPin = cJSON_GetObjectItem(pay, "pricePerPin"),
                  *perMiB = cJSON_GetObjectItem(pay, "pricePerMiBMonth"), *retention = cJSON_GetObjectItem(pay, "retentionSeconds");
            if (asset && cJSON_IsString(asset) && strlen(asset->valuestring) == 42
                && hpplite_hex_to_bytes(asset->valuestring + 2, s->asset, 20) >= 0
                && network && cJSON_IsString(network) && strncmp(network->valuestring, "eip155:", 7) == 0) {
                s->payChainId = strtoull(network->valuestring + 7, NULL, 10);
                s->pricePerByte = (perByte && cJSON_IsNumber(perByte)) ? (uint64_t)perByte->valuedouble : 0;
                s->pricePerPin = (perPin && cJSON_IsNumber(perPin)) ? (uint64_t)perPin->valuedouble : 0;
                s->pricePerMiBMonth = (perMiB && cJSON_IsNumber(perMiB) && perMiB->valuedouble > 0) ? perMiB->valuedouble : 0;
                s->retentionSeconds = (retention && cJSON_IsNumber(retention) && retention->valuedouble > 0) ? (uint64_t)retention->valuedouble : 0;
                s->hasPayment = 1;
            }
        }
        cJSON *proto = st ? cJSON_GetObjectItem(st, "protocol") : NULL;
        if (proto && cJSON_IsString(proto)) snprintf(s->protocol, sizeof s->protocol, "%s", proto->valuestring);
        cJSON *receipt = st ? cJSON_GetObjectItem(st, "receipt") : NULL;
        cJSON *domain = receipt ? cJSON_GetObjectItem(receipt, "domain") : NULL;
        if (strcmp(s->protocol, "hpplite-store/2") == 0) {
            /* /2: the store's domain, the bucket terms; prices for the status row */
            cJSON *bk = cJSON_GetObjectItem(st, "buckets"), *ad = bk ? cJSON_GetObjectItem(bk, "assetDomain") : NULL;
            const char *zAsset = jstr(bk, "asset"), *zSpender = jstr(bk, "spender");
            s->v2 = 1;
            domain = cJSON_GetObjectItem(st, "domain");
            if (zAsset && strlen(zAsset) == 42 && hpplite_hex_to_bytes(zAsset + 2, s->asset, 20) >= 0) s->hasPayment = 1;
            if (zSpender && strlen(zSpender) == 42) hpplite_hex_to_bytes(zSpender + 2, s->spender, 20);
            snprintf(s->assetName, sizeof s->assetName, "%s", jstr(ad, "name") ? jstr(ad, "name") : "Bridged USDC");
            snprintf(s->assetVersion, sizeof s->assetVersion, "%s", jstr(ad, "version") ? jstr(ad, "version") : "2");
            s->pricePerWrite = jnum(bk, "pricePerWrite");
            {
                cJSON *pm = bk ? cJSON_GetObjectItem(bk, "pricePerMiBMonth") : NULL;
                s->pricePerMiBMonth = (pm && cJSON_IsNumber(pm) && pm->valuedouble > 0) ? pm->valuedouble : 0;
            }
            s->pricePerPin = s->pricePerWrite;
            s->retentionSeconds = 30 * 24 * 3600;      /* a write's month of storage, for the status row's quote */
            {
                cJSON *cid = domain ? cJSON_GetObjectItem(domain, "chainId") : NULL;
                if (cid && cJSON_IsNumber(cid)) s->payChainId = (uint64_t)cid->valuedouble;
            }
        }
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
    return code;
}

HppliteStore *hpplite_store_open(const char *url, const uint8_t *privkey, uint64_t cap) {
    HppliteStore *s;
    size_t n;
    if (!url) return NULL;
    /* http(s)://host[:port] of an hpplite-store/2 store (or a relay, #126); kubo:// and s3:// were removed in v0.8 (#250) */
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return NULL;
    s = calloc(1, sizeof *s);
    if (s) s->lastStatus = -1;
    if (!s) return NULL;
    s->url = strdup(url);
    n = strlen(s->url);
    while (n > 0 && s->url[n - 1] == '/') s->url[--n] = 0;
    s->base = sqlite3_mprintf("%s", s->url);
    if (!s->base) { free(s->url); free(s); return NULL; }
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
            deriveFund(s, privkey);
        }
    }
    s->coveredUntil = -1;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    {
        statusRead(s);
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

/* The HTTP status of the last request since the last call: -1 none, 0 no answer, else the status. The lookup by
** name (#229) tells a store that did not answer (a reason not to create a rollup) from one that has no such
** bytes (404: gone, no reason to wait). Only a pin store (hpplite-store/2) speaks HTTP this way; others say -1. */
long hpplite_store_take_status(HppliteStore *s) {
    long st = s ? s->lastStatus : -1;
    if (s) s->lastStatus = -1;
    return st;
}

void hpplite_store_close(HppliteStore *s) {
    if (!s) return;
    if (s->crypto) hpplite_crypto_free(s->crypto);
    memset(&s->kp, 0, sizeof s->kp);
    free(s->url);
    sqlite3_free(s->base);
    free(s);
}

void hpplite_store_set_valid_for(HppliteStore *s, uint64_t seconds) { if (s && seconds) s->validFor = seconds; }

/* === hpplite-store/2 (#214, doc/store-protocol.md): the rollup's bucket and its funding wallet ===
** The funding key is derived from the node's (keccak256("hpplite funding key" || key)): backing up
** the node's key backs up the wallet too. The node binds the bucket with both keys, gives the store
** a permit to pull from the funding wallet (HPPLITE_STORE_PERMIT) and pays any 402 from it. */

static void lowerHex(const unsigned char *b, size_t n, char *out) {
    size_t i;
    out[0] = '0'; out[1] = 'x';
    hpplite_bytes_to_hex(b, n, out + 2);
    for (i = 2; i < 2 + 2 * n; i++) if (out[i] >= 'A' && out[i] <= 'F') out[i] += 32;
}

static void deriveFund(HppliteStore *s, const uint8_t privkey[32]) {
    static const char zTag[] = "hpplite funding key";
    unsigned char in[sizeof zTag - 1 + 32], k[32];
    int i;
    memcpy(in, zTag, sizeof zTag - 1);
    memcpy(in + sizeof zTag - 1, privkey, 32);
    hpplite_keccak256(in, sizeof in, k);
    for (i = 0; i < 4 && hpplite_crypto_keypair_from_privkey(s->crypto, &s->fundKp, k) != 0; i++) hpplite_keccak256(k, 32, k);
    if (i < 4) { memcpy(s->fundAddr, s->fundKp.address, 20); s->hasFund = 1; }
    memset(in, 0, sizeof in);
    memset(k, 0, sizeof k);
}

static int signWith(HppliteStore *s, HppliteKeypair *kp, const unsigned char digest[32], unsigned char sig[65]) {
    HppliteSignature hs;
    unsigned char v;
    if (hpplite_crypto_sign(s->crypto, kp, digest, &hs) != 0) return -1;
    hpplite_sig_to_eth_format(&hs, &v, sig, sig + 32);
    sig[64] = v;
    return 0;
}

/* ("HPPLiteStore", "2", chainId, payTo) and a struct of words: the digest the store recovers */
static void storeDigest(HppliteStore *s, const char *zType, const unsigned char *words, size_t nWords, unsigned char out[32]) {
    unsigned char domain[32], c[32], buf[32 * 8], h[32];
    memset(c, 0, 32); memcpy(c + 12, s->payTo, 20);
    domainOf("HPPLiteStore", "2", s->chainId, c, domain);
    hpplite_keccak256((const unsigned char *)zType, strlen(zType), buf);
    memcpy(buf + 32, words, 32 * nWords);
    hpplite_keccak256(buf, 32 * (nWords + 1), h);
    typedHash(domain, h, out);
}

static long getJson(HppliteStore *s, const char *path, cJSON **pOut) {
    Buf out = {0};
    long status = storeRequest(s, "GET", path, NULL, 0, NULL, &out, NULL, NULL);
    *pOut = (status == 200 && out.p) ? cJSON_Parse(out.p) : NULL;
    free(out.p);
    return status;
}

static long postJson(HppliteStore *s, const char *path, cJSON *body, Buf *out) {
    char *z = cJSON_PrintUnformatted(body);
    long status;
    if (!z) return 0;
    status = storeRequest(s, "POST", path, (const uint8_t *)z, strlen(z), "Content-Type: application/json", out, NULL, NULL);
    free(z);
    return status;
}

/* The permit: given when there is none, it nears its deadline (30 days) or its allowance runs low (a quarter) */
static void ensurePermit(HppliteStore *s) {
    char fund[43], path[80], sigHex[133], num[32];
    cJSON *a = NULL, *p, *body;
    uint64_t now = (uint64_t)time(NULL), deadline = now + 365ull * 86400, nonce;
    unsigned char w[32 * 5], domain[32], c[32], h[32], digest[32], sig[65];
    static const char zPermit[] = "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)";
    Buf out = {0};
    if (s->external || !s->permitCap || !s->hasFund || (s->pricePerWrite == 0 && s->pricePerMiBMonth == 0)) return;
    if (s->permitChecked && now - (uint64_t)s->permitChecked < 3600) return;
    s->permitChecked = (time_t)now;
    lowerHex(s->fundAddr, 20, fund);
    snprintf(path, sizeof path, "/account/%s", fund);
    if (getJson(s, path, &a) != 200 && a == NULL) {
        /* no account yet: nothing given; the nonce is the chain's first */
    }
    p = a ? cJSON_GetObjectItem(a, "permit") : NULL;
    if (p && cJSON_IsObject(p)) {
        cJSON *sent = cJSON_GetObjectItem(p, "sent");
        uint64_t value = jnum(p, "value"), dl = jnum(p, "deadline"), allow = jnum(a, "allowance");
        int ok = dl > now + 30ull * 86400 && value >= s->permitCap;
        if (ok && sent && cJSON_IsTrue(sent) && allow < s->permitCap / 4) ok = 0;
        if (ok) { cJSON_Delete(a); return; }
    }
    nonce = a ? jnum(a, "permitNonce") : 0;
    cJSON_Delete(a);
    memset(c, 0, 32); memcpy(c + 12, s->asset, 20);
    domainOf(s->assetName, s->assetVersion, s->chainId, c, domain);
    hpplite_keccak256((const unsigned char *)zPermit, sizeof zPermit - 1, w);
    memset(w + 32, 0, 32); memcpy(w + 44, s->fundAddr, 20);
    memset(w + 64, 0, 32); memcpy(w + 76, s->spender, 20);
    wordInt(s->permitCap, w + 96);
    wordInt(nonce, w + 128);
    {
        unsigned char ww[32 * 6];
        memcpy(ww, w, 160);
        wordInt(deadline, ww + 160);
        hpplite_keccak256(ww, 192, h);
    }
    typedHash(domain, h, digest);
    if (signWith(s, &s->fundKp, digest, sig) != 0) return;
    lowerHex(sig, 65, sigHex);
    body = cJSON_CreateObject();
    snprintf(num, sizeof num, "%llu", (unsigned long long)s->permitCap);
    cJSON_AddStringToObject(body, "value", num);
    snprintf(num, sizeof num, "%llu", (unsigned long long)deadline);
    cJSON_AddStringToObject(body, "deadline", num);
    cJSON_AddStringToObject(body, "signature", sigHex);
    snprintf(path, sizeof path, "/account/%s/permit", fund);
    if (postJson(s, path, body, &out) != 200) {
        fprintf(stderr, "hpplite: the store did not take the funding wallet's permit: %.200s\n", out.p ? out.p : "no answer");
        s->permitChecked = 0;
    }
    cJSON_Delete(body);
    free(out.p);
}

/* The bucket bound to this node's funding wallet: asked, and bound with both keys when it is not. 0 when bound */
static int ensureBucket(HppliteStore *s) {
    char da[43], fund[43], path[80], sigHex[133];
    cJSON *b = NULL, *body;
    time_t now = time(NULL);
    Buf out = {0};
    long status;
    if (!s->v2) return 0;
    if (!s->hasBucket || !s->hasKey || !(s->hasFund || s->external)) {
        snprintf(s->error, sizeof s->error, "the store keeps buckets (hpplite-store/2): %s",
                 !s->hasKey ? "this node has no key to sign for its bucket" : "the rollup's address is not known yet");
        return -1;
    }
    if (s->bound && now - s->boundChecked < 600) { ensurePermit(s); return 0; }
    lowerHex(s->bucketDa, 20, da);
    lowerHex(s->fundAddr, 20, fund);
    snprintf(path, sizeof path, "/bucket/%s", da);
    status = getJson(s, path, &b);
    if (status == 200 && b) {
        const char *f = jstr(b, "funder"), *st = jstr(b, "state");
        if (st) snprintf(s->bucketState, sizeof s->bucketState, "%s", st);
        if (cJSON_GetObjectItem(b, "coveredUntil")) {
            uint64_t cu = jnum(b, "coveredUntil");
            s->coveredUntil = (int64_t)(cu > (uint64_t)INT64_MAX ? (uint64_t)INT64_MAX : cu);
        }
        s->bound = f && strcasecmp(f, fund) == 0 && st && strcmp(st, "deleted") != 0;
        if (!s->bound && s->external) {
            /* funder= (#272): the platform binds; this node never does. Said as the funder's to lift */
            snprintf(s->error, sizeof s->error, "waiting for the funder %s: the bucket is %s%s (the node binds nothing with funder=)", fund,
                     st && strcmp(st, "deleted") == 0 ? "deleted" : f ? "bound to " : "not bound to it", st && strcmp(st, "deleted") != 0 && f ? f : "");
        }
    } else {
        s->bound = 0;
        if (s->external) snprintf(s->error, sizeof s->error, "waiting for the funder %s: the bucket is not bound%s (the node binds nothing with funder=)", fund, status == 0 ? "; the store does not answer" : "");
    }
    cJSON_Delete(b);
    if (!s->bound && s->external) {
        s->funderWait = 1;
        s->lastFailure = status == 0 ? HPPLITE_STORE_FAIL_UNREACHABLE : HPPLITE_STORE_FAIL_OTHER;
        return -1;
    }
    s->funderWait = 0;
    if (!s->bound) {
        unsigned char words[96], digest[32], sig[65];
        uint64_t issued = (uint64_t)now;
        memset(words, 0, 96);
        memcpy(words + 12, s->bucketDa, 20);
        memcpy(words + 44, s->fundAddr, 20);
        wordInt(issued, words + 64);
        storeDigest(s, "Bind(address da,address funder,uint256 issuedAt)", words, 3, digest);
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "funder", fund);
        cJSON_AddNumberToObject(body, "issuedAt", (double)issued);
        if (signWith(s, &s->kp, digest, sig) == 0) { lowerHex(sig, 65, sigHex); cJSON_AddStringToObject(body, "sequencerSignature", sigHex); }
        if (signWith(s, &s->fundKp, digest, sig) == 0) { lowerHex(sig, 65, sigHex); cJSON_AddStringToObject(body, "funderSignature", sigHex); }
        status = postJson(s, path, body, &out);
        cJSON_Delete(body);
        if (status != 200) {
            snprintf(s->error, sizeof s->error, "the store did not bind the rollup's bucket: %ld %.160s", status, out.p ? out.p : "");
            s->lastFailure = status == 0 ? HPPLITE_STORE_FAIL_UNREACHABLE : HPPLITE_STORE_FAIL_OTHER;
            free(out.p);
            return -1;
        }
        {
            cJSON *r = out.p ? cJSON_Parse(out.p) : NULL;
            const char *st = jstr(r, "state");
            if (st) snprintf(s->bucketState, sizeof s->bucketState, "%s", st);
            cJSON_Delete(r);
        }
        free(out.p);
        s->bound = 1;
    }
    s->boundChecked = now;
    ensurePermit(s);
    return 0;
}

int hpplite_store_prune(HppliteStore *s, uint64_t height) {
    unsigned char words[96], digest[32], sig[65];
    char da[43], path[128], sigHex[133];
    cJSON *body;
    Buf out = {0};
    long status;
    uint64_t issued = (uint64_t)time(NULL);
    if (!s || !s->v2 || !s->hasBucket || !s->hasKey || height == 0) return -1;
    if (ensureBucket(s) != 0) return -1;
    lowerHex(s->bucketDa, 20, da);
    snprintf(path, sizeof path, "/bucket/%s/prune", da);
    memset(words, 0, 96);
    memcpy(words + 12, s->bucketDa, 20);
    wordInt(height, words + 32);
    wordInt(issued, words + 64);
    storeDigest(s, "Prune(address da,uint256 height,uint256 issuedAt)", words, 3, digest);
    if (signWith(s, &s->kp, digest, sig) != 0) { snprintf(s->error, sizeof s->error, "the prune could not be signed"); return -1; }
    lowerHex(sig, 65, sigHex);
    body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "height", (double)height);
    cJSON_AddNumberToObject(body, "issuedAt", (double)issued);
    cJSON_AddStringToObject(body, "signature", sigHex);
    status = postJson(s, path, body, &out);
    cJSON_Delete(body);
    if (status != 200) {
        snprintf(s->error, sizeof s->error, "the store did not take the prune at %llu: %ld %.160s", (unsigned long long)height, status, out.p ? out.p : "");
        free(out.p);
        return -1;
    }
    free(out.p);
    return 0;
}

void hpplite_store_set_bucket(HppliteStore *s, const unsigned char da[20], uint64_t permitCap) {
    if (!s || !da) return;
    if (!s->hasBucket || memcmp(s->bucketDa, da, 20) != 0) s->bound = 0;
    memcpy(s->bucketDa, da, 20);
    s->hasBucket = 1;
    s->permitCap = permitCap;
}

int hpplite_store_is_bucket(HppliteStore *s) { return s && s->v2; }

int hpplite_store_funder(HppliteStore *s, unsigned char out[20]) {
    if (!s || !s->v2 || !(s->hasFund || s->external)) return -1;
    memcpy(out, s->fundAddr, 20);
    return 0;
}

int64_t hpplite_store_covered_until(HppliteStore *s) { return s && s->v2 ? s->coveredUntil : -1; }
void hpplite_store_set_funder(HppliteStore *s, const unsigned char funder[20]) {
    if (!s || !funder) return;
    memcpy(s->fundAddr, funder, 20);
    memset(&s->fundKp, 0, sizeof s->fundKp);
    s->hasFund = 0;                     /* no funding key here: the platform's signer binds and permits */
    s->external = 1;
    s->bound = 0;
}
int hpplite_store_waiting_on_funder(HppliteStore *s) { return s && s->funderWait; }
int hpplite_store_bind_digest(HppliteStore *s, const unsigned char funder[20], uint64_t issuedAt, unsigned char digest[32]) {
    unsigned char words[96];
    if (!s || !funder || !digest || !s->hasBucket) return -1;
    if (!s->hasSigner && hpplite_store_take_status(s) != 200) return -1;
    memset(words, 0, 96);
    memcpy(words + 12, s->bucketDa, 20);
    memcpy(words + 44, funder, 20);
    wordInt(issuedAt, words + 64);
    storeDigest(s, "Bind(address da,address funder,uint256 issuedAt)", words, 3, digest);
    return 0;
}
int hpplite_store_sign_bind(HppliteStore *s, const unsigned char funder[20], uint64_t issuedAt, unsigned char sig[65]) {
    unsigned char digest[32];
    if (!s || !s->hasKey || hpplite_store_bind_digest(s, funder, issuedAt, digest) != 0) return -1;
    return signWith(s, &s->kp, digest, sig);
}
const char *hpplite_store_bucket_state(HppliteStore *s) { return s && s->v2 && s->bucketState[0] ? s->bucketState : NULL; }

/* The bucket statement in its compact form (HPPLITE_STATEMENT_SIZE), checked against the store's signer */
/* A bucket statement as an anchor carries it: its fields, and whether its signature recovers to the signer it names
** over BucketStatement(da, height, pruneHeight, coveredUntil, issuedAt) in ("HPPLiteStore", "2", chainId, payTo).
** 0 verified, -1 not (or not a version 2 statement) */
int hpplite_store_statement_verify(const uint8_t *st, size_t n, uint64_t chainId, const unsigned char da[20],
                                   HppliteStatement *out) {
    static const char zType[] = "BucketStatement(address da,uint256 height,uint256 pruneHeight,uint256 coveredUntil,uint256 issuedAt)";
    unsigned char domain[32], c[32], buf[32 * 6], h[32], digest[32], pub[33], who[20];
    uint64_t f[4] = {0, 0, 0, 0};
    HppliteSignature hs;
    HppliteCrypto *crypto;
    int i, k, rc;
    if (out) memset(out, 0, sizeof *out);
    if (!st || n != HPPLITE_STATEMENT_SIZE || st[0] != 2 || !da) return -1;
    for (k = 0; k < 4; k++) for (i = 0; i < 8; i++) f[k] = (f[k] << 8) | st[41 + 8 * k + i];
    if (out) {
        memcpy(out->signer, st + 1, 20); memcpy(out->payTo, st + 21, 20);
        out->height = f[0]; out->pruneHeight = f[1]; out->coveredUntil = f[2]; out->issuedAt = f[3];
    }
    memset(c, 0, 32); memcpy(c + 12, st + 21, 20);
    domainOf("HPPLiteStore", "2", chainId, c, domain);
    hpplite_keccak256((const unsigned char *)zType, sizeof zType - 1, buf);
    memset(buf + 32, 0, 32); memcpy(buf + 44, da, 20);
    for (k = 0; k < 4; k++) wordInt(f[k], buf + 64 + 32 * k);
    hpplite_keccak256(buf, sizeof buf, h);
    typedHash(domain, h, digest);
    memcpy(hs.sig, st + 73, 64);
    hs.recid = st[137] >= 27 ? st[137] - 27 : st[137];
    if (hs.recid > 1 || !(crypto = hpplite_crypto_init())) return -1;
    rc = hpplite_crypto_recover_pubkey(crypto, digest, &hs, pub);
    hpplite_crypto_free(crypto);
    if (rc != 0 || eth_address_from_compressed_pubkey(pub, who) != 0) return -1;
    return memcmp(who, st + 1, 20) == 0 ? 0 : -1;
}

int hpplite_store_statement(HppliteStore *s, uint8_t out[HPPLITE_STATEMENT_SIZE]) {
    char da[43], path[80];
    cJSON *b = NULL, *st;
    unsigned char words[160], digest[32], sig[65], who[20], pub[33];
    uint64_t height, prune, covered, issued;
    const char *zSig, *zState;
    HppliteSignature hs;
    int i;
    if (!s || !s->v2 || !s->hasBucket || !s->hasSigner) return -1;
    lowerHex(s->bucketDa, 20, da);
    snprintf(path, sizeof path, "/bucket/%s", da);
    if (getJson(s, path, &b) != 200 || !b) { snprintf(s->error, sizeof s->error, "the store gave no bucket statement"); cJSON_Delete(b); return -1; }
    zState = jstr(b, "state");
    if (zState) snprintf(s->bucketState, sizeof s->bucketState, "%s", zState);
    st = cJSON_GetObjectItem(b, "statement");
    zSig = jstr(st, "signature");
    height = jnum(st, "height"); prune = jnum(st, "pruneHeight"); covered = jnum(st, "coveredUntil"); issued = jnum(st, "issuedAt");
    s->coveredUntil = (int64_t)(covered > (uint64_t)INT64_MAX ? (uint64_t)INT64_MAX : covered);
    if (!zSig || strlen(zSig) != 132 || hpplite_hex_to_bytes(zSig + 2, sig, 65) < 0) { cJSON_Delete(b); snprintf(s->error, sizeof s->error, "the bucket statement is unsigned"); return -1; }
    cJSON_Delete(b);
    memset(words, 0, 32); memcpy(words + 12, s->bucketDa, 20);
    wordInt(height, words + 32); wordInt(prune, words + 64); wordInt(covered, words + 96); wordInt(issued, words + 128);
    storeDigest(s, "BucketStatement(address da,uint256 height,uint256 pruneHeight,uint256 coveredUntil,uint256 issuedAt)", words, 5, digest);
    memcpy(hs.sig, sig, 64);
    hs.recid = sig[64] >= 27 ? sig[64] - 27 : sig[64];
    if (hs.recid > 1 || hpplite_crypto_recover_pubkey(s->crypto, digest, &hs, pub) != 0
        || eth_address_from_compressed_pubkey(pub, who) != 0 || memcmp(who, s->signer, 20) != 0) {
        snprintf(s->error, sizeof s->error, "the bucket statement does not verify against the store's published signer");
        return -1;
    }
    /* version 2 | signer | payTo | height | pruneHeight | coveredUntil | issuedAt (8 each, big-endian) | signature */
    out[0] = 2;
    memcpy(out + 1, s->signer, 20);
    memcpy(out + 21, s->payTo, 20);
    for (i = 0; i < 8; i++) {
        out[41 + i] = (uint8_t)(height >> (56 - 8 * i));
        out[49 + i] = (uint8_t)(prune >> (56 - 8 * i));
        out[57 + i] = (uint8_t)(covered >> (56 - 8 * i));
        out[65 + i] = (uint8_t)(issued >> (56 - 8 * i));
    }
    memcpy(out + 73, sig, 65);
    return HPPLITE_STATEMENT_SIZE;
}

int hpplite_store_set_key(HppliteStore *s, const uint8_t privkey[32]) {
    if (!s || !privkey || !s->crypto) return -1;
    if (hpplite_crypto_keypair_from_privkey(s->crypto, &s->kp, privkey) != 0) return -1;
    memcpy(s->address, s->kp.address, 20);
    s->hasKey = 1;
    if (!s->external) deriveFund(s, privkey);
    s->bound = 0;
    return 0;
}
const char *hpplite_store_error(HppliteStore *s) { return s ? s->error : "no store"; }
const char *hpplite_store_url(HppliteStore *s) { return s ? s->url : NULL; }
uint64_t hpplite_store_spent(HppliteStore *s, int *pCalls) { if (pCalls) *pCalls = s ? s->calls : 0; return s ? s->spent : 0; }
int hpplite_store_calls(HppliteStore *s) { return s ? s->nOk : 0; }
size_t hpplite_store_last_bytes(HppliteStore *s) { return s ? s->lastBytes : 0; }

uint64_t hpplite_store_quote(HppliteStore *s, size_t nbytes) {
    double storage;
    if (!s || !s->hasPayment) return 0;
    /* as the reference service computes it (operators/store price_put), in the same order:
    ** the storage for the retention bought, rounded up to a whole unit */
    storage = s->pricePerMiBMonth * (double)nbytes / (1024.0 * 1024.0) * ((double)s->retentionSeconds / (30.0 * 24 * 3600));
    return s->pricePerPin + s->pricePerByte * (uint64_t)nbytes + (uint64_t)ceil(storage - 1e-9);
}

int hpplite_store_payment(HppliteStore *s, unsigned char asset[20], uint64_t *pChainId, uint64_t *pPerByte, uint64_t *pPerPin) {
    if (!s || !s->hasPayment) return -1;
    if (asset) memcpy(asset, s->asset, 20);
    if (pChainId) *pChainId = s->payChainId;
    if (pPerByte) *pPerByte = s->pricePerByte;
    if (pPerPin) *pPerPin = s->pricePerPin;
    return 0;
}

void hpplite_store_set_relay(HppliteStore *s) { if (s) s->isRelay = 1; }
void hpplite_store_set_spend(HppliteStore *s, struct HppliteSpend *sp) { if (s) s->spend = sp; }

/* The protocols this build speaks (doc/store-protocol.md, doc/relay-protocol.md, #188): a service that names
** another - a later version, or the other kind (a relay given as store=) - is written nothing. One that names
** none predates #188 and is taken as version 1. 0 when it may be written to, else -1 with the reason in s->error */
#define HPPLITE_STORE_PROTOCOL 2
#define HPPLITE_RELAY_PROTOCOL 1
static int protocolOk(HppliteStore *s) {
    const char *want = s->isRelay ? "hpplite-relay" : "hpplite-store";
    int max = s->isRelay ? HPPLITE_RELAY_PROTOCOL : HPPLITE_STORE_PROTOCOL;
    size_t n = strlen(want);
    const char *v;
    if (!s->protocol[0] && !s->isRelay && statusRead(s) == 0) {
        /* not answering at open, nor now: the chain may carry the bytes meanwhile (storefallback) */
        s->lastFailure = HPPLITE_STORE_FAIL_UNREACHABLE;
        return -1;
    }
    if (!s->protocol[0]) {
        if (s->isRelay) return 0;                       /* a relay from before #188 speaks version 1 */
        snprintf(s->error, sizeof s->error, "the store at %s names no protocol: this build speaks hpplite-store/2", s->url);
        s->lastFailure = HPPLITE_STORE_FAIL_OTHER;
        return -1;
    }
    v = strncmp(s->protocol, want, n) == 0 && s->protocol[n] == '/' ? s->protocol + n + 1 : NULL;
    if (v && atoi(v) >= (s->isRelay ? 1 : 2) && atoi(v) <= max) return 0;   /* hpplite-store/1 is gone (#214) */
    if (v && atoi(v) < max && !s->isRelay)
        snprintf(s->error, sizeof s->error, "the store at %s speaks %s; this build speaks %s/%d only (buckets)", s->url, s->protocol, want, max);
    else if (v) snprintf(s->error, sizeof s->error, "the %s at %s speaks %s; this build speaks up to %s/%d: upgrade HPPLite",
                    s->isRelay ? "relay" : "store", s->url, s->protocol, want, max);
    else snprintf(s->error, sizeof s->error, "%s speaks %s, not %s: is it a %s?", s->url, s->protocol, want,
                  s->isRelay ? "store given as relay=" : "relay given as store=");
    s->lastFailure = HPPLITE_STORE_FAIL_OTHER;
    return -1;
}

/* A POST the provider may charge for: a 402 paid in x402's exact scheme, once */
static long paidPost(HppliteStore *s, const char *path, const uint8_t *body, size_t len, const char *zHeader, Buf *out) {
    char *zRequired = NULL, *zResponse = NULL;
    long status = storeRequest(s, "POST", path, body, len, zHeader, out, &zRequired, NULL);
    if (status == 402 && zRequired) {
        uint64_t amount = 0;
        char *zPay = storePay(s, zRequired, &amount), *zAgain = NULL;
        if (zPay) {
            free(out->p); out->p = NULL; out->n = 0;
            if (zHeader) { char *z = sqlite3_mprintf("%s\n%s", zHeader, zPay); sqlite3_free(zPay); zPay = z; }
            status = storeRequest(s, "POST", path, body, len, zPay, out, &zAgain, &zResponse);
            sqlite3_free(zPay);
            sqlite3_free(zRequired);
            zRequired = zAgain;
            if (status == 200) {
                s->funderWait = 0;
                s->spent += amount;
                s->calls++;
                hpplite_spend_add(s->spend, amount);
            } else if (status == 402 && zRequired) {
                size_t n = 0;
                unsigned char *j = b64decode(zRequired, &n);
                cJSON *r = j ? cJSON_Parse((const char *)j) : NULL;
                const char *e = jstr(r, "error");
                if (e) snprintf(s->error, sizeof s->error, "the %s refused the payment: %.200s", s->isRelay ? "relay" : "store", e);
                cJSON_Delete(r);
                free(j);
            }
        }
    }
    sqlite3_free(zRequired);
    sqlite3_free(zResponse);
    return status;
}

long hpplite_store_paid_post(HppliteStore *s, const char *path, const uint8_t *body, size_t len, char **pzOut) {
    Buf out = {0};
    char *zRequired = NULL, *zResponse = NULL;
    long status;
    if (pzOut) *pzOut = NULL;
    if (!s || !path || !body) return 0;
    s->error[0] = 0;
    if (protocolOk(s) != 0) return 0;
    (void)zRequired; (void)zResponse;
    status = paidPost(s, path, body, len, NULL, &out);
    if (status == 200) s->nOk++;
    if (pzOut && out.p) *pzOut = sqlite3_mprintf("%.*s", (int)out.n, out.p);
    free(out.p);
    return status;
}

char *hpplite_store_put(HppliteStore *s, const uint8_t *data, size_t len) {
    return hpplite_store_put_at(s, 0, data, len);
}

char *hpplite_store_put_at(HppliteStore *s, uint64_t height, const uint8_t *data, size_t len) {
    Buf out = {0};
    char *zRequired = NULL, *zResponse = NULL, *zRef = NULL, *zHeader = NULL, path[160];
    long status;
    if (!s || !data || !len) return NULL;
    s->error[0] = 0;
    s->lastFailure = HPPLITE_STORE_FAIL_NONE;
    if (protocolOk(s) != 0) return NULL;
    s->lastBytes = len;
    (void)zRequired; (void)zResponse;
    snprintf(path, sizeof path, "/put");
    if (s->v2) {
        /* /2: Content(da, height, keccak256(bytes)) signed by the sequencer, into the rollup's bucket */
        unsigned char words[96], digest[32], sig[65];
        char da[43], sigHex[133];
        if (ensureBucket(s) != 0) return NULL;
        memset(words, 0, 32); memcpy(words + 12, s->bucketDa, 20);
        wordInt(height, words + 32);
        hpplite_keccak256(data, len, words + 64);
        storeDigest(s, "Content(address da,uint256 height,bytes32 hash)", words, 3, digest);
        if (signWith(s, &s->kp, digest, sig) != 0) { snprintf(s->error, sizeof s->error, "could not sign the put"); return NULL; }
        lowerHex(s->bucketDa, 20, da);
        lowerHex(sig, 65, sigHex);
        snprintf(path, sizeof path, "/put?bucket=%s&height=%llu", da, (unsigned long long)height);
        zHeader = sqlite3_mprintf("X-HPPLite-Signature: %s", sigHex);
    }
    status = paidPost(s, path, data, len, zHeader, &out);
    sqlite3_free(zHeader);
    if (status == 402 && (strstr(s->error, "HPPLITE_STORE_CAP") || strstr(s->error, "HPPLITE_SPEND_CAP"))) {
        s->lastFailure = HPPLITE_STORE_FAIL_CAP;            /* nothing signed past the caps: the batch waits */
        free(out.p);
        return NULL;
    }
    if (status != 200) {
        if (!s->error[0]) snprintf(s->error, sizeof s->error, "store answered %ld to put: %.120s", status, out.p ? out.p : "");
        s->lastFailure = status == 0 ? HPPLITE_STORE_FAIL_UNREACHABLE : HPPLITE_STORE_FAIL_OTHER;
        free(out.p);
        return NULL;
    }
    {
        cJSON *r = out.p ? cJSON_Parse(out.p) : NULL;
        cJSON *cid = r ? cJSON_GetObjectItem(r, "cid") : NULL;
        if (cid && cJSON_IsString(cid) && cid->valuestring[0]) {
            zRef = sqlite3_mprintf("%s", cid->valuestring);
        } else {
            snprintf(s->error, sizeof s->error, "store's answer to put names no cid");
            s->lastFailure = HPPLITE_STORE_FAIL_OTHER;
        }
        cJSON_Delete(r);
    }
    free(out.p);
    if (zRef) s->nOk++;
    return zRef;
}

/* What HPPLiteDA.batchDigest() computes (DA version 3): the sequencer's claim about one height */
static void batchDigest(const unsigned char da[20], uint64_t chainId, const unsigned char instance[32], uint64_t height,
                        const uint8_t *data, size_t len, unsigned char digest[32]) {
    static const char zDomainType[] = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
    static const char zBatchType[] = "Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)";
    unsigned char w[32 * 5], domain[32], h[32], pre[66];
    hpplite_keccak256((const unsigned char *)zDomainType, sizeof(zDomainType) - 1, w);
    hpplite_keccak256((const unsigned char *)"HPPLiteDA", 9, w + 32);
    hpplite_keccak256((const unsigned char *)"3", 1, w + 64);
    wordInt(chainId, w + 96);
    memset(w + 128, 0, 32); memcpy(w + 128 + 12, da, 20);
    hpplite_keccak256(w, 160, domain);
    hpplite_keccak256((const unsigned char *)zBatchType, sizeof(zBatchType) - 1, w);
    memcpy(w + 32, instance, 32);
    wordInt(height, w + 64);
    hpplite_keccak256(data, len, w + 96);
    hpplite_keccak256(w, 128, h);
    pre[0] = 0x19; pre[1] = 0x01; memcpy(pre + 2, domain, 32); memcpy(pre + 34, h, 32);
    hpplite_keccak256(pre, 66, digest);
}

int hpplite_store_batch_signer(const unsigned char da[20], uint64_t chainId, const unsigned char instance[32], uint64_t height,
                               const uint8_t *data, size_t len, const uint8_t sig[65], unsigned char out[20]) {
    unsigned char digest[32], pub[33];
    HppliteSignature hs;
    HppliteCrypto *crypto;
    int rc;
    if (!da || !instance || !data || !sig || !out) return -1;
    batchDigest(da, chainId, instance, height, data, len, digest);
    memcpy(hs.sig, sig, 64);
    hs.recid = sig[64] >= 27 ? sig[64] - 27 : sig[64];
    if (hs.recid > 1) return -1;
    crypto = hpplite_crypto_init();
    if (!crypto) return -1;
    rc = hpplite_crypto_recover_pubkey(crypto, digest, &hs, pub);
    hpplite_crypto_free(crypto);
    if (rc != 0 || eth_address_from_compressed_pubkey(pub, out) != 0) return -1;
    return 0;
}

int hpplite_store_sign_batch(HppliteStore *s, const unsigned char da[20], uint64_t chainId, const unsigned char instance[32],
                             uint64_t height, const uint8_t *data, size_t len, uint8_t sig[65]) {
    unsigned char digest[32];
    HppliteSignature hs;
    unsigned char v;
    if (!s || !s->hasKey || !da || !instance || !data || !sig) return -1;
    batchDigest(da, chainId, instance, height, data, len, digest);
    if (hpplite_crypto_sign(s->crypto, &s->kp, digest, &hs) != 0) return -1;
    hpplite_sig_to_eth_format(&hs, &v, sig, sig + 32);
    sig[64] = v;
    return 0;
}

static int nameOk(const char *zName) {
    const char *p = zName;
    int n = 0;
    if (!zName) return 0;
    while (*p && *p != '-') { if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return 0; p++; n++; }
    if (n != 40 || *p != '-') return 0;
    for (p++, n = 0; *p; p++, n++) if (*p < '0' || *p > '9') return 0;
    return n >= 1 && n <= 18;
}

int hpplite_store_put_named(HppliteStore *s, const char *zName, const unsigned char instance[32], const uint8_t *data, size_t len, const uint8_t sig[65]) {
    uint8_t *body;
    Buf out = {0};
    long status;
    int rc = -1;
    if (!s || !nameOk(zName) || !instance || !data || !len || !sig) return -1;
    s->error[0] = 0;
    if (protocolOk(s) != 0) return -1;
    body = malloc(len + 97);
    if (!body) return -1;
    memcpy(body, data, len);
    memcpy(body + len, instance, 32);
    memcpy(body + len + 32, sig, 65);
    {
        char path[120];
        snprintf(path, sizeof path, "/name/%s", zName);
        if (s->v2 && ensureBucket(s) != 0) status = -1;                 /* /2: a write into the bucket, paid as a put (#214) */
        else status = s->v2 ? paidPost(s, path, body, len + 97, NULL, &out) : storeRequest(s, "POST", path, body, len + 97, NULL, &out, NULL, NULL);
        if (status == 200) rc = 0;
        else if (status == -1) { /* the reason is in s->error */ }
        else if (!s->error[0]) snprintf(s->error, sizeof s->error, "store answered %ld to the name %s: %.120s", status, zName, out.p ? out.p : "");
    }
    free(body);
    free(out.p);
    return rc;
}

uint8_t *hpplite_store_get_named(HppliteStore *s, const char *zName, size_t *pLen, uint8_t sig[65], unsigned char instance[32]) {
    Buf out = {0};
    long status;
    if (pLen) *pLen = 0;
    if (!s || !nameOk(zName)) return NULL;
    s->error[0] = 0;
    {
        char path[120];
        snprintf(path, sizeof path, "/name/%s", zName);
        status = storeRequest(s, "GET", path, NULL, 0, NULL, &out, NULL, NULL);
    }
    if (status != 200 || !out.p || out.n < 98) {
        if (!s->error[0]) snprintf(s->error, sizeof s->error, "store answered %ld to the name %s", status, zName);
        free(out.p);
        return NULL;
    }
    out.n -= 97;
    if (instance) memcpy(instance, out.p + out.n, 32);
    if (sig) memcpy(sig, out.p + out.n + 32, 65);
    out.p[out.n] = 0;
    if (pLen) *pLen = out.n;
    return (uint8_t *)out.p;
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
    if (out.p) s->nOk++;
    return (uint8_t *)out.p;                      /* malloc'd, terminated */
}
