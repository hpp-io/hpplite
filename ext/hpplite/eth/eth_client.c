/*
** Minimal Ethereum JSON-RPC client for HPPLite
*/

#include "eth_client.h"
#include "cJSON.h"
#include "keccak256.h"
#include "rlp.h"
#include <curl/curl.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>

struct EthClient {
    pthread_mutex_t lock;      /* one curl easy handle: one request at a time (timer thread vs app thread) */
    char *rpc_url;
    uint64_t chain_id;
    uint8_t privkey[32];
    uint8_t address[20];
    int has_privkey;           /* can sign: a raw key, or a signer */
    EthSignFn signFn;          /* ...the signer, when the key is elsewhere (#17) */
    void *signArg;
    char error[256];
    CURL *curl;
    secp256k1_context *secp_ctx;
    time_t downUntil;          /* an endpoint that timed out: calls fail at once until then */
};

/* One request may take this long. An endpoint that answers nothing in 15 s
** is down for our purposes; HPP's RPC answers in well under a second, a
** full batch transaction included. HPPLITE_RPC_TIMEOUT (seconds) overrides. */
static long rpcTimeout(void) {
    const char *z = getenv("HPPLITE_RPC_TIMEOUT");
    long v = z ? strtol(z, NULL, 10) : 0;
    return v > 0 ? v : 15;
}

/* Response buffer for curl */
struct ResponseBuffer {
    char *data;
    size_t size;
};

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct ResponseBuffer *buf = (struct ResponseBuffer *)userp;
    
    char *ptr = realloc(buf->data, buf->size + realsize + 1);
    if (!ptr) return 0;
    
    buf->data = ptr;
    memcpy(&(buf->data[buf->size]), contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = 0;
    
    return realsize;
}

/* Hex utilities */
void eth_hex_encode_to(const uint8_t *data, size_t len, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[i * 2] = digits[(data[i] >> 4) & 0xf];
        out[i * 2 + 1] = digits[data[i] & 0xf];
    }
    out[len * 2] = '\0';
}

char *eth_hex_encode(const uint8_t *data, size_t len) {
    char *hex = malloc(2 + len * 2 + 1);
    if (!hex) return NULL;
    hex[0] = '0';
    hex[1] = 'x';
    eth_hex_encode_to(data, len, hex + 2);
    return hex;
}

static int hex_digit(int ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int eth_hex_decode_any(const char *hex, uint8_t *out, size_t max) {
    size_t n, i;
    if (!hex) return -1;
    if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex += 2;
    n = strlen(hex);
    if (n % 2 != 0 || n / 2 > max) return -1;
    for (i = 0; i < n / 2; i++) {
        int hi = hex_digit((unsigned char)hex[i * 2]), lo = hex_digit((unsigned char)hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)(n / 2);
}

/* Exactly out_len bytes: 0, or -1 */
int eth_hex_decode(const char *hex, uint8_t *out, size_t out_len) {
    return eth_hex_decode_any(hex, out, out_len) == (int)out_len ? 0 : -1;
}

/* Make JSON-RPC call */
static cJSON *rpc_call_locked(EthClient *client, const char *method, cJSON *params);
/*
** A curl easy handle must not be used by two threads at once. HPPLite's
** timer thread and the application (hpplite_get_l1(), close) both reach
** the same client, which crashed the process once both were busy.
*/
static cJSON *rpc_call(EthClient *client, const char *method, cJSON *params) {
    cJSON *r;
    pthread_mutex_lock(&client->lock);
    r = rpc_call_locked(client, method, params);
    pthread_mutex_unlock(&client->lock);
    return r;
}

static cJSON *rpc_call_locked(EthClient *client, const char *method, cJSON *params) {
    struct ResponseBuffer response = {0};
    struct curl_slist *headers = NULL;

    /* After a timeout, the operations that follow (a lease read is several
    ** calls, a flush and a close several more) fail at once instead of each
    ** waiting out its own timeout: a frozen endpoint costs one timeout, not
    ** one per call. The window is short; the node retries on its own schedule. */
    if (client->downUntil && time(NULL) < client->downUntil) {
        snprintf(client->error, sizeof(client->error), "L1 RPC did not answer recently; not trying again yet");
        if (params) cJSON_Delete(params);
        return NULL;
    }
    
    /* Build request */
    cJSON *request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "jsonrpc", "2.0");
    cJSON_AddStringToObject(request, "method", method);
    cJSON_AddItemToObject(request, "params", params ? params : cJSON_CreateArray());
    cJSON_AddNumberToObject(request, "id", 1);
    
    char *request_str = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);
    
    /* Setup curl */
    curl_easy_reset(client->curl);
    headers = curl_slist_append(headers, "Content-Type: application/json");
    
    curl_easy_setopt(client->curl, CURLOPT_URL, client->rpc_url);
    if (getenv("HPPLITE_CA_BUNDLE")) curl_easy_setopt(client->curl, CURLOPT_CAINFO, getenv("HPPLITE_CA_BUNDLE"));   /* the certificates to trust: a static binary has no system bundle (#269) */
    curl_easy_setopt(client->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(client->curl, CURLOPT_POSTFIELDS, request_str);
    curl_easy_setopt(client->curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(client->curl, CURLOPT_WRITEDATA, &response);
    
    curl_easy_setopt(client->curl, CURLOPT_USERAGENT, "hpplite/0.1");
    curl_easy_setopt(client->curl, CURLOPT_TIMEOUT, rpcTimeout());
    curl_easy_setopt(client->curl, CURLOPT_CONNECTTIMEOUT, 5L);

    /* An RPC under load answers 429/5xx, sometimes with an empty body; one
    ** empty body used to reach cJSON_Parse(NULL) and crash the process. */
    CURLcode res = CURLE_OK;
    long httpStatus = 0;
    int attempt;
    for (attempt = 0; attempt < 4; attempt++) {
        free(response.data);
        response.data = NULL; response.size = 0;
        res = curl_easy_perform(client->curl);
        httpStatus = 0;
        if (res == CURLE_OK) curl_easy_getinfo(client->curl, CURLINFO_RESPONSE_CODE, &httpStatus);
        if (res == CURLE_OK && httpStatus != 429 && httpStatus < 500) break;
        /* an endpoint that does not answer will not answer 2 s later either */
        if (res == CURLE_OPERATION_TIMEDOUT) break;
        usleep((useconds_t)(250000u << attempt));            /* 0.25, 0.5, 1, 2 s */
    }
    curl_slist_free_all(headers);
    free(request_str);
    
    if (res == CURLE_OPERATION_TIMEDOUT) client->downUntil = time(NULL) + 10;
    else client->downUntil = 0;
    if (res != CURLE_OK) {
        snprintf(client->error, sizeof(client->error), "curl error: %s", 
                 curl_easy_strerror(res));
        free(response.data);
        return NULL;
    }
    if (!response.data || httpStatus >= 400) {
        snprintf(client->error, sizeof(client->error), "RPC HTTP %ld%s", httpStatus,
                 response.data ? "" : " (empty body)");
        if (getenv("HPPLITE_ETH_DEBUG")) fprintf(stderr, "eth: %s -> %s\n", method, client->error);
        free(response.data);
        return NULL;
    }
    
    /* Parse response */
    cJSON *json = cJSON_Parse(response.data);
    free(response.data);
    
    if (!json) {
        snprintf(client->error, sizeof(client->error), "JSON parse error");
        return NULL;
    }
    
    /* Check for error */
    cJSON *error = cJSON_GetObjectItem(json, "error");
    if (error) {
        cJSON *msg = cJSON_GetObjectItem(error, "message");
        snprintf(client->error, sizeof(client->error), "RPC error: %s",
                 msg ? msg->valuestring : "unknown");
        cJSON_Delete(json);
        return NULL;
    }
    
    return json;
}

/*
** Process-wide one-time setup. curl_global_init() is not thread-safe and
** curl_easy_init() would call it implicitly from whichever thread comes
** first; the static secp256k1 contexts used for address derivation were
** created lazily without a lock.
*/
#include <pthread.h>
static pthread_once_t g_eth_once = PTHREAD_ONCE_INIT;
static secp256k1_context *g_addr_ctx = NULL;   /* VERIFY: pubkey -> address */
static secp256k1_context *g_sign_ctx = NULL;   /* SIGN: privkey -> pubkey */
static void eth_global_init(void) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    g_addr_ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
    g_sign_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
}
void eth_client_global_init(void) {
    pthread_once(&g_eth_once, eth_global_init);
}

EthClient *eth_client_create(const char *rpc_url, uint64_t chain_id) {
    EthClient *client;
    eth_client_global_init();
    client = calloc(1, sizeof(EthClient));
    if (!client) return NULL;
    pthread_mutex_init(&client->lock, NULL);
    
    client->rpc_url = strdup(rpc_url);
    client->chain_id = chain_id;
    client->curl = curl_easy_init();
    client->secp_ctx = secp256k1_context_create(
        SECP256K1_CONTEXT_SIGN | SECP256K1_CONTEXT_VERIFY);
    
    if (!client->curl || !client->secp_ctx) {
        eth_client_destroy(client);
        return NULL;
    }
    
    return client;
}

void eth_client_destroy(EthClient *client) {
    if (!client) return;
    pthread_mutex_destroy(&client->lock);
    free(client->rpc_url);
    if (client->curl) curl_easy_cleanup(client->curl);
    if (client->secp_ctx) secp256k1_context_destroy(client->secp_ctx);
    free(client);
}

int eth_client_set_privkey(EthClient *client, const uint8_t privkey[32]) {
    memcpy(client->privkey, privkey, 32);
    client->signFn = NULL;
    client->signArg = NULL;
    
    /* Derive address from private key */
    secp256k1_pubkey pubkey;
    if (!secp256k1_ec_pubkey_create(client->secp_ctx, &pubkey, privkey)) {
        snprintf(client->error, sizeof(client->error), "invalid private key");
        return -1;
    }
    
    uint8_t pubkey_serialized[65];
    size_t pubkey_len = 65;
    secp256k1_ec_pubkey_serialize(client->secp_ctx, pubkey_serialized, &pubkey_len,
                                   &pubkey, SECP256K1_EC_UNCOMPRESSED);
    
    eth_address_from_pubkey(pubkey_serialized, client->address);
    client->has_privkey = 1;
    return 0;
}

int eth_client_sign_hash(EthClient *client, const uint8_t hash[32], uint8_t sig[65]) {
    int recid;
    if (!client || !client->has_privkey) return -1;
    if (client->signFn) {
        if (client->signFn(client->signArg, hash, sig, &recid) != 0 || recid < 0 || recid > 1) return -1;
    } else {
        secp256k1_ecdsa_recoverable_signature rs;
        if (!secp256k1_ecdsa_sign_recoverable(client->secp_ctx, &rs, hash, client->privkey, NULL, NULL)) return -1;
        secp256k1_ecdsa_recoverable_signature_serialize_compact(client->secp_ctx, sig, &recid, &rs);
    }
    sig[64] = (uint8_t)(27 + recid);
    return 0;
}

int eth_client_set_signer(EthClient *client, EthSignFn fn, void *arg, const uint8_t address[20]) {
    if (!client || !fn || !address) return -1;
    memset(client->privkey, 0, 32);
    memcpy(client->address, address, 20);
    client->signFn = fn;
    client->signArg = arg;
    client->has_privkey = 1;
    return 0;
}

uint64_t eth_client_chain_id(EthClient *client) {
    return client->chain_id;
}

const char *eth_client_error(EthClient *client) {
    return client->error;
}

uint8_t *eth_client_call(EthClient *client,
                         const uint8_t to[20],
                         const uint8_t *data, size_t data_len,
                         size_t *result_len) {
    cJSON *params = cJSON_CreateArray();
    cJSON *tx = cJSON_CreateObject();
    
    char *to_hex = eth_hex_encode(to, 20);
    char *data_hex = eth_hex_encode(data, data_len);
    
    cJSON_AddStringToObject(tx, "to", to_hex);
    cJSON_AddStringToObject(tx, "data", data_hex);
    cJSON_AddItemToArray(params, tx);
    cJSON_AddItemToArray(params, cJSON_CreateString("latest"));
    
    free(to_hex);
    free(data_hex);
    
    cJSON *response = rpc_call(client, "eth_call", params);
    if (!response) return NULL;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || !result->valuestring) {
        snprintf(client->error, sizeof(client->error), "no result in response");
        cJSON_Delete(response);
        return NULL;
    }
    
    /* Decode hex result */
    const char *hex = result->valuestring;
    if (hex[0] == '0' && hex[1] == 'x') hex += 2;
    
    size_t len = strlen(hex) / 2;
    uint8_t *out = malloc(len);
    if (!out) {
        cJSON_Delete(response);
        return NULL;
    }
    
    for (size_t i = 0; i < len; i++) {
        unsigned int byte;
        sscanf(hex + i * 2, "%02x", &byte);
        out[i] = (uint8_t)byte;
    }
    
    *result_len = len;
    cJSON_Delete(response);
    return out;
}

/*
** Timestamp and number of the latest block: the chain's clock. Lease expiry
** is in chain time, which is not wall-clock time on a test chain (anvil
** after evm_increaseTime) and may drift on a real one.
*/
int eth_client_latest_block(EthClient *client, uint64_t *pNumber, uint64_t *pTimestamp) {
    cJSON *params = cJSON_CreateArray();
    cJSON_AddItemToArray(params, cJSON_CreateString("latest"));
    cJSON_AddItemToArray(params, cJSON_CreateFalse());
    cJSON *response = rpc_call(client, "eth_getBlockByNumber", params);
    if (!response) return -1;
    cJSON *result = cJSON_GetObjectItem(response, "result");
    cJSON *num = result ? cJSON_GetObjectItem(result, "number") : NULL;
    cJSON *ts = result ? cJSON_GetObjectItem(result, "timestamp") : NULL;
    if (!num || !ts || !cJSON_IsString(num) || !cJSON_IsString(ts)) { cJSON_Delete(response); return -1; }
    if (pNumber) *pNumber = strtoull(num->valuestring, NULL, 16);
    if (pTimestamp) *pTimestamp = strtoull(ts->valuestring, NULL, 16);
    cJSON_Delete(response);
    return 0;
}

/*
** eth_estimateGas for a call from our address; (uint64_t)-1 on failure.
** Storage-heavy calls (submitBatch) cannot be sized by a per-byte formula
** across chains; the node estimates and adds a margin.
*/
uint64_t eth_client_estimate_gas(EthClient *client, const uint8_t to[20], const uint8_t *data, size_t data_len) {
    if (!client || !client->has_privkey) return (uint64_t)-1;
    cJSON *tx = cJSON_CreateObject();
    char *from_hex = eth_hex_encode(client->address, 20);
    char *to_hex = eth_hex_encode(to, 20);
    char *data_hex = eth_hex_encode(data, data_len);
    cJSON_AddStringToObject(tx, "from", from_hex);
    cJSON_AddStringToObject(tx, "to", to_hex);
    cJSON_AddStringToObject(tx, "data", data_hex);
    free(from_hex); free(to_hex); free(data_hex);
    cJSON *params = cJSON_CreateArray();
    cJSON_AddItemToArray(params, tx);
    cJSON *response = rpc_call(client, "eth_estimateGas", params);
    if (!response) return (uint64_t)-1;
    cJSON *result = cJSON_GetObjectItem(response, "result");
    uint64_t gas = (result && cJSON_IsString(result)) ? strtoull(result->valuestring, NULL, 16) : (uint64_t)-1;
    cJSON_Delete(response);
    return gas;
}

uint64_t eth_client_get_nonce(EthClient *client, const uint8_t address[20]) {
    cJSON *params = cJSON_CreateArray();
    char *addr_hex = eth_hex_encode(address, 20);
    cJSON_AddItemToArray(params, cJSON_CreateString(addr_hex));
    cJSON_AddItemToArray(params, cJSON_CreateString("pending"));
    free(addr_hex);
    
    cJSON *response = rpc_call(client, "eth_getTransactionCount", params);
    if (!response) return (uint64_t)-1;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || !cJSON_IsString(result)) {
        snprintf(client->error, sizeof(client->error), "eth_getTransactionCount: no result");
        cJSON_Delete(response);
        return (uint64_t)-1;
    }
    uint64_t nonce = strtoull(result->valuestring, NULL, 16);
    cJSON_Delete(response);
    return nonce;
}

int eth_client_has_balance(EthClient *client, const uint8_t address[20]) {
    cJSON *params = cJSON_CreateArray(), *response, *result;
    char *addr_hex = eth_hex_encode(address, 20);
    const char *p;
    int has = -1;
    cJSON_AddItemToArray(params, cJSON_CreateString(addr_hex));
    cJSON_AddItemToArray(params, cJSON_CreateString("latest"));
    free(addr_hex);
    response = rpc_call(client, "eth_getBalance", params);
    if (!response) return -1;
    result = cJSON_GetObjectItem(response, "result");
    if (result && cJSON_IsString(result) && strncmp(result->valuestring, "0x", 2) == 0) {
        for (p = result->valuestring + 2; *p == '0'; p++) {}
        has = *p != 0;
    }
    cJSON_Delete(response);
    return has;
}

/* eth_getBalance in wei, as a double (exact to 2^53 wei; a warning's arithmetic needs no more): 0, or -1 */
int eth_client_balance(EthClient *client, const uint8_t address[20], double *pWei) {
    cJSON *params = cJSON_CreateArray(), *response, *result;
    char *addr_hex = eth_hex_encode(address, 20);
    int rc = -1;
    cJSON_AddItemToArray(params, cJSON_CreateString(addr_hex));
    cJSON_AddItemToArray(params, cJSON_CreateString("latest"));
    free(addr_hex);
    response = rpc_call(client, "eth_getBalance", params);
    if (!response) return -1;
    result = cJSON_GetObjectItem(response, "result");
    if (result && cJSON_IsString(result) && strncmp(result->valuestring, "0x", 2) == 0) {
        double v = 0;
        for (const char *p = result->valuestring + 2; *p; p++) {
            int d = (*p >= '0' && *p <= '9') ? *p - '0' : (*p >= 'a' && *p <= 'f') ? *p - 'a' + 10 : (*p >= 'A' && *p <= 'F') ? *p - 'A' + 10 : -1;
            if (d < 0) { v = -1; break; }
            v = v * 16 + d;
        }
        if (v >= 0) { *pWei = v; rc = 0; }
    }
    cJSON_Delete(response);
    return rc;
}

uint64_t eth_client_gas_price(EthClient *client) {
    cJSON *response = rpc_call(client, "eth_gasPrice", NULL);
    if (!response) return (uint64_t)-1;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || !cJSON_IsString(result)) {
        snprintf(client->error, sizeof(client->error), "eth_gasPrice: no result");
        cJSON_Delete(response);
        return (uint64_t)-1;
    }
    uint64_t price = strtoull(result->valuestring, NULL, 16);
    cJSON_Delete(response);
    return price;
}

/* Sign transaction using EIP-155 */
static size_t sign_transaction(EthClient *client,
                               uint64_t nonce, uint64_t gas_price, uint64_t gas_limit,
                               const uint8_t to[20], uint64_t value,
                               const uint8_t *data, size_t data_len,
                               uint8_t *out) {
    /* Build unsigned tx for signing (EIP-155)
     * Buffer size: data_len + ~200 bytes for other fields + signature
     */
    size_t items_size = data_len + 256;
    if (items_size < 2048) items_size = 2048;
    uint8_t *items = malloc(items_size);
    if (!items) return 0;
    size_t items_len = 0;
    
    items_len += rlp_encode_uint64(nonce, items + items_len);
    items_len += rlp_encode_uint64(gas_price, items + items_len);
    items_len += rlp_encode_uint64(gas_limit, items + items_len);
    items_len += rlp_encode_bytes(to, 20, items + items_len);
    items_len += rlp_encode_uint64(value, items + items_len);
    items_len += rlp_encode_bytes(data, data_len, items + items_len);
    /* EIP-155: append chain_id, 0, 0 */
    items_len += rlp_encode_uint64(client->chain_id, items + items_len);
    items_len += rlp_encode_uint64(0, items + items_len);
    items_len += rlp_encode_uint64(0, items + items_len);
    
    /* RLP encode as list - use dynamic buffer for larger data */
    size_t unsigned_tx_size = items_len + 16;  /* items + list header */
    uint8_t *unsigned_tx = malloc(unsigned_tx_size);
    if (!unsigned_tx) {
        free(items);
        return 0;
    }
    size_t header_len = rlp_encode_list_header(items_len, unsigned_tx);
    memcpy(unsigned_tx + header_len, items, items_len);
    size_t unsigned_tx_len = header_len + items_len;

    /* Hash for signing */
    uint8_t hash[32];
    keccak256(unsigned_tx, unsigned_tx_len, hash);
    free(unsigned_tx);  /* Done with unsigned tx */

    /* Sign: the signer when the key is elsewhere (#17), else the raw key */
    uint8_t sig_bytes[64];
    int recid;
    if (client->signFn) {
        if (client->signFn(client->signArg, hash, sig_bytes, &recid) != 0 || recid < 0 || recid > 3) {
            snprintf(client->error, sizeof(client->error), "the signer did not sign");
            free(items);
            return 0;
        }
    } else {
        secp256k1_ecdsa_recoverable_signature sig;
        if (!secp256k1_ecdsa_sign_recoverable(client->secp_ctx, &sig, hash,
                                               client->privkey, NULL, NULL)) {
            free(items);
            return 0;
        }
        secp256k1_ecdsa_recoverable_signature_serialize_compact(client->secp_ctx,
                                                                 sig_bytes, &recid, &sig);
    }
    
    /* Build signed tx */
    items_len = 0;
    items_len += rlp_encode_uint64(nonce, items + items_len);
    items_len += rlp_encode_uint64(gas_price, items + items_len);
    items_len += rlp_encode_uint64(gas_limit, items + items_len);
    items_len += rlp_encode_bytes(to, 20, items + items_len);
    items_len += rlp_encode_uint64(value, items + items_len);
    items_len += rlp_encode_bytes(data, data_len, items + items_len);
    
    /* EIP-155 v = chain_id * 2 + 35 + recid */
    uint64_t v = client->chain_id * 2 + 35 + recid;
    items_len += rlp_encode_uint64(v, items + items_len);
    /* r and s are integers: RLP wants the minimal big-endian form. Encoding
    ** them as fixed 32-byte strings made every signature whose r or s starts
    ** with a zero byte (about 1 in 128) an invalid transaction ("Failed to
    ** decode transaction"), seen as sporadic unconfirmed batches. */
    items_len += rlp_encode_uint256(sig_bytes, items + items_len);        /* r */
    items_len += rlp_encode_uint256(sig_bytes + 32, items + items_len);   /* s */
    
    header_len = rlp_encode_list_header(items_len, out);
    memcpy(out + header_len, items, items_len);
    size_t result = header_len + items_len;
    free(items);
    return result;
}

size_t eth_client_sign_tx(EthClient *client, uint64_t nonce, uint64_t gas_price, uint64_t gas_limit,
                          const uint8_t to[20], uint64_t value, const uint8_t *data, size_t data_len,
                          uint8_t *out, size_t out_size) {
    /* sign_transaction sizes its own scratch from data_len; give it that much */
    uint8_t *raw = malloc(data_len + 512);
    size_t n;
    if (!raw) return 0;
    n = sign_transaction(client, nonce, gas_price, gas_limit, to, value, data, data_len, raw);
    if (n > 0 && n <= out_size) memcpy(out, raw, n); else n = 0;
    free(raw);
    return n;
}

uint8_t *eth_client_send_raw_tx(EthClient *client,
                                const uint8_t *raw_tx, size_t tx_len) {
    cJSON *params = cJSON_CreateArray();
    char *tx_hex = eth_hex_encode(raw_tx, tx_len);
    cJSON_AddItemToArray(params, cJSON_CreateString(tx_hex));
    free(tx_hex);
    
    cJSON *response = rpc_call(client, "eth_sendRawTransaction", params);
    if (!response) return NULL;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || !result->valuestring) {
        snprintf(client->error, sizeof(client->error), "no tx hash in response");
        cJSON_Delete(response);
        return NULL;
    }
    
    uint8_t *tx_hash = malloc(32);
    if (!tx_hash || eth_hex_decode(result->valuestring, tx_hash, 32) != 0) {
        snprintf(client->error, sizeof(client->error), "malformed tx hash in response");
        free(tx_hash);
        cJSON_Delete(response);
        return NULL;
    }
    cJSON_Delete(response);
    return tx_hash;
}

static uint8_t *eth_client_send_tx_locked(EthClient *client,
                                          const uint8_t to[20], const uint8_t *data, size_t data_len,
                                          uint64_t gas_limit, uint64_t value);
/* nonce fetch, sign and send as one unit: two threads must not pick the same nonce */
static pthread_mutex_t g_send_lock = PTHREAD_MUTEX_INITIALIZER;
uint8_t *eth_client_send_tx(EthClient *client, const uint8_t to[20], const uint8_t *data, size_t data_len,
                            uint64_t gas_limit, uint64_t value) {
    uint8_t *r;
    pthread_mutex_lock(&g_send_lock);
    r = eth_client_send_tx_locked(client, to, data, data_len, gas_limit, value);
    pthread_mutex_unlock(&g_send_lock);
    return r;
}

static uint8_t *eth_client_send_tx_locked(EthClient *client,
                            const uint8_t to[20],
                            const uint8_t *data, size_t data_len,
                            uint64_t gas_limit,
                            uint64_t value) {
    if (!client->has_privkey) {
        snprintf(client->error, sizeof(client->error), "no private key set");
        return NULL;
    }
    
    uint64_t nonce = eth_client_get_nonce(client, client->address);
    if (nonce == (uint64_t)-1) return NULL;
    
    uint64_t gas_price = eth_client_gas_price(client);
    if (gas_price == (uint64_t)-1) return NULL;
    
    /* Add 10% to gas price */
    gas_price = gas_price + gas_price / 10;

    /* Allocate buffer for signed tx: data + overhead for RLP encoding + signature */
    size_t raw_tx_size = data_len + 512;
    if (raw_tx_size < 4096) raw_tx_size = 4096;
    uint8_t *raw_tx = malloc(raw_tx_size);
    if (!raw_tx) {
        snprintf(client->error, sizeof(client->error), "malloc failed");
        return NULL;
    }

    size_t raw_tx_len = sign_transaction(client, nonce, gas_price, gas_limit,
                                          to, value, data, data_len, raw_tx);
    if (raw_tx_len == 0) {
        snprintf(client->error, sizeof(client->error), "failed to sign transaction");
        free(raw_tx);
        return NULL;
    }

    uint8_t *result = eth_client_send_raw_tx(client, raw_tx, raw_tx_len);
    if (getenv("HPPLITE_ETH_DEBUG")) {
        char *hex = eth_hex_encode(raw_tx, raw_tx_len);
        fprintf(stderr, "eth: nonce=%llu gasPrice=%llu gas=%llu data=%zuB -> %s%s%s\n",
                (unsigned long long)nonce, (unsigned long long)gas_price, (unsigned long long)gas_limit, data_len,
                result ? "ok" : "FAILED ", result ? "" : client->error, result ? "" : "\n     raw ");
        if (!result) fprintf(stderr, "%s\n", hex);
        free(hex);
    }
    free(raw_tx);
    return result;
}

int eth_client_get_receipt(EthClient *client,
                           const uint8_t tx_hash[32],
                           int *status) {
    cJSON *params = cJSON_CreateArray();
    char *hash_hex = eth_hex_encode(tx_hash, 32);
    cJSON_AddItemToArray(params, cJSON_CreateString(hash_hex));
    free(hash_hex);
    
    cJSON *response = rpc_call(client, "eth_getTransactionReceipt", params);
    if (!response) return -1;
    
    cJSON *result = cJSON_GetObjectItem(response, "result");
    if (!result || cJSON_IsNull(result)) {
        cJSON_Delete(response);
        return 0; /* pending */
    }
    
    cJSON *status_json = cJSON_GetObjectItem(result, "status");
    if (status_json && status_json->valuestring) {
        *status = (strcmp(status_json->valuestring, "0x1") == 0) ? 1 : 0;
    }
    
    cJSON_Delete(response);
    return 1; /* mined */
}

int eth_client_receipt_topic(EthClient *client, const uint8_t tx_hash[32],
                             const uint8_t topic0[32], int index, uint8_t out[32]) {
    cJSON *params = cJSON_CreateArray();
    char *hash_hex = eth_hex_encode(tx_hash, 32), *want = eth_hex_encode(topic0, 32);
    cJSON *response, *result, *logs, *log;
    int found = 0;
    cJSON_AddItemToArray(params, cJSON_CreateString(hash_hex));
    free(hash_hex);
    response = rpc_call(client, "eth_getTransactionReceipt", params);
    if (!response) { free(want); return -2; }         /* the RPC, not the transaction: ask again (#68) */
    result = cJSON_GetObjectItem(response, "result");
    if (!result || cJSON_IsNull(result)) { cJSON_Delete(response); free(want); return 0; }
    logs = cJSON_GetObjectItem(result, "logs");
    cJSON_ArrayForEach(log, logs) {
        cJSON *topics = cJSON_GetObjectItem(log, "topics");
        cJSON *t0 = topics ? cJSON_GetArrayItem(topics, 0) : NULL;
        cJSON *ti = topics ? cJSON_GetArrayItem(topics, index) : NULL;
        if (t0 && t0->valuestring && ti && ti->valuestring && strcasecmp(t0->valuestring, want) == 0
            && eth_hex_decode(ti->valuestring, out, 32) == 0) {
            found = 1;
            break;
        }
    }
    cJSON_Delete(response);
    free(want);
    return found ? 1 : -1;
}

uint8_t *eth_client_get_log(EthClient *client, const uint8_t address[20], uint64_t block,
                            const uint8_t topic0[32], const uint8_t topic1[32],
                            size_t *data_len, uint8_t topic2[32]) {
    cJSON *params = cJSON_CreateArray(), *filter = cJSON_CreateObject(), *topics = cJSON_CreateArray();
    cJSON *response, *result, *log;
    char *addr_hex = eth_hex_encode(address, 20), *t0 = eth_hex_encode(topic0, 32), *t1 = eth_hex_encode(topic1, 32);
    char block_hex[32];
    uint8_t *out = NULL;
    if (data_len) *data_len = 0;
    snprintf(block_hex, sizeof block_hex, "0x%llx", (unsigned long long)block);
    cJSON_AddStringToObject(filter, "address", addr_hex);
    cJSON_AddStringToObject(filter, "fromBlock", block_hex);
    cJSON_AddStringToObject(filter, "toBlock", block_hex);
    cJSON_AddItemToArray(topics, cJSON_CreateString(t0));
    cJSON_AddItemToArray(topics, cJSON_CreateString(t1));
    cJSON_AddItemToObject(filter, "topics", topics);
    cJSON_AddItemToArray(params, filter);
    free(addr_hex); free(t0); free(t1);
    response = rpc_call(client, "eth_getLogs", params);
    if (!response) return NULL;
    result = cJSON_GetObjectItem(response, "result");
    if (!result || !cJSON_IsArray(result)) {
        snprintf(client->error, sizeof(client->error), "no result in response");
        cJSON_Delete(response);
        return NULL;
    }
    cJSON_ArrayForEach(log, result) {
        cJSON *data = cJSON_GetObjectItem(log, "data");
        cJSON *tp = cJSON_GetObjectItem(log, "topics");
        cJSON *t2 = tp ? cJSON_GetArrayItem(tp, 2) : NULL;
        const char *hex;
        size_t len;
        if (!data || !cJSON_IsString(data)) continue;
        hex = data->valuestring;
        if (hex[0] == '0' && hex[1] == 'x') hex += 2;
        len = strlen(hex) / 2;
        out = malloc(len + 1);
        if (!out) break;
        if (eth_hex_decode(data->valuestring, out, len) != 0 && len > 0) { free(out); out = NULL; break; }
        out[len] = 0;
        if (data_len) *data_len = len;
        if (topic2) {
            if (!t2 || !cJSON_IsString(t2) || eth_hex_decode(t2->valuestring, topic2, 32) != 0) memset(topic2, 0, 32);
        }
        break;
    }
    if (!out) snprintf(client->error, sizeof(client->error), "no such log in block %llu", (unsigned long long)block);
    cJSON_Delete(response);
    return out;
}

int eth_client_get_block_logs(EthClient *client, const uint8_t address[20], uint64_t block, EthLog **pLogs, int *pnLogs) {
    cJSON *params = cJSON_CreateArray(), *filter = cJSON_CreateObject();
    cJSON *response, *result, *log;
    char *addr_hex = eth_hex_encode(address, 20);
    char block_hex[32];
    EthLog *out = NULL;
    int n = 0, cap = 0, ok = 0;
    *pLogs = NULL; *pnLogs = 0;
    snprintf(block_hex, sizeof block_hex, "0x%llx", (unsigned long long)block);
    cJSON_AddStringToObject(filter, "address", addr_hex);
    cJSON_AddStringToObject(filter, "fromBlock", block_hex);
    cJSON_AddStringToObject(filter, "toBlock", block_hex);
    cJSON_AddItemToArray(params, filter);
    free(addr_hex);
    response = rpc_call(client, "eth_getLogs", params);
    if (!response) return -1;
    result = cJSON_GetObjectItem(response, "result");
    if (!result || !cJSON_IsArray(result)) {
        snprintf(client->error, sizeof(client->error), "no result in response");
        cJSON_Delete(response);
        return -1;
    }
    ok = 1;
    cJSON_ArrayForEach(log, result) {
        cJSON *data = cJSON_GetObjectItem(log, "data");
        cJSON *tp = cJSON_GetObjectItem(log, "topics");
        cJSON *t;
        EthLog *e;
        const char *hex;
        size_t len;
        if (!data || !cJSON_IsString(data) || !tp || !cJSON_IsArray(tp)) continue;
        if (n == cap) {
            EthLog *grown = realloc(out, (size_t)(cap ? cap * 2 : 8) * sizeof(EthLog));
            if (!grown) { ok = 0; break; }
            out = grown; cap = cap ? cap * 2 : 8;
        }
        e = &out[n];
        memset(e, 0, sizeof *e);
        cJSON_ArrayForEach(t, tp) {
            if (e->nTopics >= 4 || !cJSON_IsString(t)) break;
            if (eth_hex_decode(t->valuestring, e->topics[e->nTopics], 32) != 0) break;
            e->nTopics++;
        }
        hex = data->valuestring;
        if (hex[0] == '0' && hex[1] == 'x') hex += 2;
        len = strlen(hex) / 2;
        e->data = malloc(len + 1);
        if (!e->data) { ok = 0; break; }
        if (len > 0 && eth_hex_decode(data->valuestring, e->data, len) != 0) { free(e->data); ok = 0; break; }
        e->data[len] = 0;
        e->nData = len;
        n++;
    }
    cJSON_Delete(response);
    if (!ok) { eth_logs_free(out, n); snprintf(client->error, sizeof(client->error), "malformed log in block %llu", (unsigned long long)block); return -1; }
    *pLogs = out; *pnLogs = n;
    return 0;
}

void eth_logs_free(EthLog *logs, int n) {
    int i;
    if (!logs) return;
    for (i = 0; i < n; i++) free(logs[i].data);
    free(logs);
}

int eth_address_from_compressed_pubkey(const uint8_t pubkey[33], uint8_t address[20]) {
    eth_client_global_init();
    if (!g_addr_ctx) return -1;

    /* Parse compressed pubkey */
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_parse(g_addr_ctx, &pk, pubkey, 33)) {
        return -1;
    }

    /* Serialize uncompressed */
    uint8_t uncompressed[65];
    size_t len = 65;
    if (!secp256k1_ec_pubkey_serialize(g_addr_ctx, uncompressed, &len,
                                        &pk, SECP256K1_EC_UNCOMPRESSED)) {
        return -1;
    }

    /* Hash to get address */
    eth_address_from_pubkey(uncompressed, address);
    return 0;
}

int eth_address_from_privkey(const uint8_t privkey[32], uint8_t address[20]) {
    secp256k1_context *sign_ctx;
    eth_client_global_init();
    sign_ctx = g_sign_ctx;
    if (!sign_ctx) return -1;

    /* Derive public key from private key */
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_create(sign_ctx, &pk, privkey)) {
        return -1;
    }

    /* Serialize uncompressed */
    uint8_t uncompressed[65];
    size_t len = 65;
    if (!secp256k1_ec_pubkey_serialize(sign_ctx, uncompressed, &len,
                                        &pk, SECP256K1_EC_UNCOMPRESSED)) {
        return -1;
    }

    /* Hash to get address */
    eth_address_from_pubkey(uncompressed, address);
    return 0;
}
