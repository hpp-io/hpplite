/*
** HPPLite directory backend (#35): the coordinator and the store in a
** directory, l1=file:///path. No chain, no wallet: Litestream-like use, the
** offline fixture for hpplite-verify, the crash harness's persistent L1.
**
** Layout, every file written to <name>.tmp, fsynced and renamed into place,
** so a crash leaves the old or the new content and never a torn file:
**   config         "checkpointInterval N\nleaseDuration N\n"
**   owner          hex wallet of the first keyed connection (forceExpire is its right)
**   lease          "wallet <hex> instance <hex> expiry <epoch>"
**   height         "N": batches 1..N are posted (written after batch-N.bin)
**   batch-N.bin    the posted bytes of batch N
**   checkpoint     "height N time T root <hex>": the latest checkpoint
**
** Nothing is cached: every call reads the files it needs, so several
** processes on one directory (a writer, replicas, hpplite-verify) see one
** state, as they see one chain. A claim serializes on lease.lock (O_EXCL);
** a lock older than DIR_LOCK_STALE seconds is a crashed claimant's and is
** removed. Time is the wall clock. Transaction hashes are synthetic and
** every "transaction" is final when the call returns.
*/

#include "l1_backend.h"
#include "hpplite_sqlite.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define DIR_DEFAULT_LEASE      300
#define DIR_DEFAULT_CHECKPOINT 100
#define DIR_BATCH_LIMIT        (16u * 1024 * 1024)   /* a sanity cap on one file; the node's own batch limit is what splits */
#define DIR_LOCK_STALE         10
#define DIR_PATH               1024

typedef struct DirL1 {
    HppliteL1 base;
    char *dir;
    int hasPrivkey;
    unsigned char myAddress[20];
    HppliteCrypto *crypto;
    HppliteL1Callback callback;
    void *callbackArg;
} DirL1;
#define self ((DirL1 *)l1)

typedef struct DirLease {
    int held;
    unsigned char wallet[20];
    unsigned char instance[32];
    uint64_t expiry;
} DirLease;

/* === Files === */

static uint64_t dirNow(void) { return (uint64_t)time(NULL); }

static void dirPath(HppliteL1 *l1, const char *name, char out[DIR_PATH]) {
    snprintf(out, DIR_PATH, "%s/%s", self->dir, name);
}

static int dirWrite(const char *path, const void *data, size_t len) {
    char tmp[DIR_PATH + 8];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { fclose(f); unlink(tmp); return -1; }
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* The file's bytes, terminated, malloc'd (the caller frees); NULL when missing */
static uint8_t *dirRead(const char *path, size_t *pLen) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (pLen) *pLen = 0;
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n + 1);
    if (buf && n && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; if (pLen) *pLen = (size_t)n; }
    return buf;
}

static int dirWriteText(HppliteL1 *l1, const char *name, const char *text) {
    char path[DIR_PATH];
    dirPath(l1, name, path);
    return dirWrite(path, text, strlen(text));
}

static char *dirReadText(HppliteL1 *l1, const char *name) {
    char path[DIR_PATH];
    dirPath(l1, name, path);
    return (char *)dirRead(path, NULL);
}

static void dirHex(const unsigned char *in, int n, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2 * i] = h[in[i] >> 4]; out[2 * i + 1] = h[in[i] & 15]; }
    out[2 * n] = 0;
}

static int dirUnhex(const char *in, unsigned char *out, int n) {
    for (int i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(in + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (unsigned char)v;
    }
    return 0;
}

/* === The records === */

static void dirReadConfig(HppliteL1 *l1, uint64_t *pInterval, uint64_t *pLease) {
    char *txt = dirReadText(l1, "config");
    unsigned long long ci = DIR_DEFAULT_CHECKPOINT, ld = DIR_DEFAULT_LEASE;
    if (txt) {
        char *p;
        if ((p = strstr(txt, "checkpointInterval ")) != NULL) sscanf(p + 19, "%llu", &ci);
        if ((p = strstr(txt, "leaseDuration ")) != NULL) sscanf(p + 14, "%llu", &ld);
        free(txt);
    }
    if (pInterval) *pInterval = ci ? ci : DIR_DEFAULT_CHECKPOINT;
    if (pLease) *pLease = ld ? ld : DIR_DEFAULT_LEASE;
}

static int dirWriteConfig(HppliteL1 *l1, uint64_t interval, uint64_t lease) {
    char buf[96];
    snprintf(buf, sizeof(buf), "checkpointInterval %llu\nleaseDuration %llu\n",
             (unsigned long long)interval, (unsigned long long)lease);
    return dirWriteText(l1, "config", buf);
}

static void dirReadLease(HppliteL1 *l1, DirLease *out) {
    char *txt = dirReadText(l1, "lease");
    char wallet[41] = {0}, inst[65] = {0};
    unsigned long long expiry = 0;
    memset(out, 0, sizeof(*out));
    if (!txt) return;
    if (sscanf(txt, "wallet %40s instance %64s expiry %llu", wallet, inst, &expiry) == 3
        && dirUnhex(wallet, out->wallet, 20) == 0 && dirUnhex(inst, out->instance, 32) == 0) {
        out->held = 1;
        out->expiry = expiry;
    }
    free(txt);
}

static int dirWriteLease(HppliteL1 *l1, const DirLease *in) {
    char wallet[41], inst[65], buf[160];
    dirHex(in->wallet, 20, wallet);
    dirHex(in->instance, 32, inst);
    snprintf(buf, sizeof(buf), "wallet %s instance %s expiry %llu\n", wallet, inst, (unsigned long long)in->expiry);
    return dirWriteText(l1, "lease", buf);
}

static int dirLeaseActive(const DirLease *lease, uint64_t now) {
    return lease->held && lease->expiry > now;
}

static int dirLeaseIsMine(const DirLease *lease, const unsigned char instanceId[32], uint64_t now) {
    return dirLeaseActive(lease, now) && memcmp(lease->instance, instanceId, 32) == 0;
}

static uint64_t dirReadHeight(HppliteL1 *l1) {
    char *txt = dirReadText(l1, "height");
    unsigned long long h = 0;
    if (txt) { sscanf(txt, "%llu", &h); free(txt); }
    return h;
}

static int dirWriteHeight(HppliteL1 *l1, uint64_t h) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%llu\n", (unsigned long long)h);
    return dirWriteText(l1, "height", buf);
}

static void dirReadCheckpoint(HppliteL1 *l1, uint64_t *pHeight, uint64_t *pTime, unsigned char root[32]) {
    char *txt = dirReadText(l1, "checkpoint");
    char hex[65] = {0};
    unsigned long long h = 0, t = 0;
    *pHeight = 0; *pTime = 0; memset(root, 0, 32);
    if (!txt) return;
    if (sscanf(txt, "height %llu time %llu root %64s", &h, &t, hex) == 3 && dirUnhex(hex, root, 32) == 0) {
        *pHeight = h; *pTime = t;
    } else {
        memset(root, 0, 32);
    }
    free(txt);
}

static int dirReadOwner(HppliteL1 *l1, unsigned char out[20]) {
    char *txt = dirReadText(l1, "owner");
    int rc = -1;
    memset(out, 0, 20);
    if (txt) { rc = dirUnhex(txt, out, 20); free(txt); }
    return rc;
}

/* The claim lock: one claimant at a time; a lock a crashed claimant left is taken over */
static int dirLock(HppliteL1 *l1) {
    char path[DIR_PATH];
    int fd, tries = 2;
    dirPath(l1, "lease.lock", path);
    while (tries-- > 0) {
        fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0) { close(fd); return 0; }
        if (errno != EEXIST) return -1;
        struct stat st;
        if (stat(path, &st) == 0 && (uint64_t)st.st_mtime + DIR_LOCK_STALE < dirNow()) { unlink(path); continue; }
        return -1;
    }
    return -1;
}

static void dirUnlock(HppliteL1 *l1) {
    char path[DIR_PATH];
    dirPath(l1, "lease.lock", path);
    unlink(path);
}

static char *dirTx(const char *what, uint64_t n) {
    return sqlite3_mprintf("0xdir-%s-%016llx-%08x", what, (unsigned long long)n, (unsigned)dirNow());
}

/* === Connections === */

static HppliteL1 *dir_connect(const char *endpoint, const char *contractAddress) {
    const char *path;
    struct stat st;
    DirL1 *p;
    (void)contractAddress;                       /* the directory is the rollup */
    if (!endpoint || strncmp(endpoint, "file://", 7) != 0) return NULL;
    path = endpoint + 7;
    if (!*path) return NULL;
    if (stat(path, &st) != 0) {
        if (mkdir(path, 0700) != 0 && errno != EEXIST) return NULL;
        if (stat(path, &st) != 0) return NULL;
    }
    if (!S_ISDIR(st.st_mode)) return NULL;
    p = calloc(1, sizeof(DirL1));
    if (!p) return NULL;
    p->base.ops = &hppliteL1DirOps;
    p->dir = strdup(path);
    p->crypto = hpplite_crypto_init();
    if (!p->dir || !p->crypto) {
        if (p->crypto) hpplite_crypto_free(p->crypto);
        free(p->dir);
        free(p);
        return NULL;
    }
    {
        /* the configuration of record, written once so the next reader finds it */
        HppliteL1 *l1 = &p->base;
        char cfgPath[DIR_PATH];
        dirPath(l1, "config", cfgPath);
        if (stat(cfgPath, &st) != 0) dirWriteConfig(l1, DIR_DEFAULT_CHECKPOINT, DIR_DEFAULT_LEASE);
    }
    return &p->base;
}

static HppliteL1 *dir_connect_config(const HppliteNetworkConfig *config) {
    return config ? dir_connect(config->rpcUrl, NULL) : NULL;
}

static HppliteL1 *dir_connect_factory(const char *rpc_url, const char *factory_address, const uint8_t privkey[32]) {
    (void)rpc_url; (void)factory_address; (void)privkey;
    return NULL;                                  /* a directory is one rollup; there is no factory */
}

static void dir_disconnect(HppliteL1 *l1) {
    if (!l1) return;
    if (self->crypto) hpplite_crypto_free(self->crypto);
    free(self->dir);
    free(self);
}

/* === Coordinator === */

static HppliteSystemConfig *dir_get_system_config(HppliteL1 *l1) {
    HppliteSystemConfig *cfg;
    if (!l1) return NULL;
    cfg = sqlite3_malloc(sizeof(HppliteSystemConfig));
    if (!cfg) return NULL;
    memset(cfg, 0, sizeof(*cfg));
    cfg->daScheme = sqlite3_mprintf("file");
    cfg->batchSizeLimit = 0;                      /* no limit of its own: the node's stands (the contract's would replace it) */
    cfg->version = 1;
    dirReadConfig(l1, &cfg->checkpointInterval, &cfg->leaseDuration);
    return cfg;
}

static char *dir_set_checkpoint_interval(HppliteL1 *l1, uint64_t interval) {
    uint64_t ci, ld;
    if (!l1 || interval == 0) return NULL;
    dirReadConfig(l1, &ci, &ld);
    if (dirWriteConfig(l1, interval, ld) != 0) return NULL;
    return dirTx("config", interval);
}

static int dir_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]) {
    HppliteKeypair kp;
    if (!l1 || !privkey) return -1;
    if (hpplite_crypto_keypair_from_privkey(self->crypto, &kp, privkey) != 0) return -1;
    memcpy(self->myAddress, kp.address, 20);
    self->hasPrivkey = 1;
    {
        /* the first keyed connection is the owner, as the factory makes a rollup's creator */
        unsigned char owner[20];
        if (dirReadOwner(l1, owner) != 0) {
            char hex[41];
            dirHex(kp.address, 20, hex);
            dirWriteText(l1, "owner", hex);
        }
    }
    return 0;
}

static HppliteL1State *dir_get_state(HppliteL1 *l1) {
    HppliteL1State *st;
    DirLease lease;
    if (!l1) return NULL;
    st = sqlite3_malloc(sizeof(HppliteL1State));
    if (!st) return NULL;
    memset(st, 0, sizeof(*st));
    dirReadOwner(l1, st->owner);
    dirReadLease(l1, &lease);
    if (lease.held) {
        memcpy(st->sequencerWallet, lease.wallet, 20);
        memcpy(st->sequencerInstance, lease.instance, 32);
        st->leaseExpiry = lease.expiry;
    }
    dirReadConfig(l1, &st->checkpointInterval, &st->leaseDuration);
    dirReadCheckpoint(l1, &st->lastCheckpointHeight, &st->lastCheckpointTime, st->lastStateRoot);
    st->currentBlock = dirReadHeight(l1);
    st->currentTimestamp = dirNow();
    return st;
}

static char *dir_claim_sequencer(HppliteL1 *l1, const unsigned char instanceId[32]) {
    DirLease lease;
    uint64_t now, ci, ld;
    if (!l1 || !instanceId) return NULL;
    if (dirLock(l1) != 0) return NULL;            /* another claimant this instant: try again later */
    now = dirNow();
    dirReadLease(l1, &lease);
    if (dirLeaseActive(&lease, now) && memcmp(lease.instance, instanceId, 32) != 0) {
        dirUnlock(l1);
        return NULL;                              /* held by another instance */
    }
    dirReadConfig(l1, &ci, &ld);
    memset(&lease, 0, sizeof(lease));
    lease.held = 1;
    if (self->hasPrivkey) memcpy(lease.wallet, self->myAddress, 20);
    memcpy(lease.instance, instanceId, 32);
    lease.expiry = now + ld;
    if (dirWriteLease(l1, &lease) != 0) { dirUnlock(l1); return NULL; }
    dirUnlock(l1);
    if (self->callback) self->callback(self->callbackArg, "SequencerClaimed", (void *)instanceId);
    return dirTx("claim", now);
}

static char *dir_renew_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    DirLease lease;
    uint64_t now, ci, ld;
    if (!l1 || !instanceId) return NULL;
    now = dirNow();
    dirReadLease(l1, &lease);
    if (!dirLeaseIsMine(&lease, instanceId, now)) return NULL;
    dirReadConfig(l1, &ci, &ld);
    lease.expiry = now + ld;
    if (dirWriteLease(l1, &lease) != 0) return NULL;
    return dirTx("renew", now);
}

static char *dir_release_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    DirLease lease;
    uint64_t now;
    if (!l1) return NULL;
    now = dirNow();
    dirReadLease(l1, &lease);
    if (!lease.held) return NULL;
    if (!instanceId) {
        /* forceExpireLease: the owner's right */
        unsigned char owner[20];
        if (!self->hasPrivkey || dirReadOwner(l1, owner) != 0 || memcmp(owner, self->myAddress, 20) != 0) return NULL;
    } else if (!dirLeaseIsMine(&lease, instanceId, now)) {
        return NULL;
    }
    lease.expiry = now;
    if (dirWriteLease(l1, &lease) != 0) return NULL;
    return dirTx("release", now);
}

static int dir_is_lease_active(HppliteL1 *l1) {
    DirLease lease;
    if (!l1) return 0;
    dirReadLease(l1, &lease);
    return dirLeaseActive(&lease, dirNow());
}

static int dir_get_addresses(HppliteL1 *l1, char contractHex[43], char daHex[43]) {
    (void)l1; (void)contractHex; (void)daHex;
    return -1;                                    /* no contracts; status shows none */
}

static int dir_get_my_address(HppliteL1 *l1, unsigned char out[20]) {
    if (!l1 || !self->hasPrivkey) return -1;
    memcpy(out, self->myAddress, 20);
    return 0;
}

static uint64_t dir_get_lease_time_remaining(HppliteL1 *l1) {
    DirLease lease;
    uint64_t now;
    if (!l1) return 0;
    now = dirNow();
    dirReadLease(l1, &lease);
    return dirLeaseActive(&lease, now) ? lease.expiry - now : 0;
}

static char *dir_submit_checkpoint(HppliteL1 *l1, const HppliteCheckpoint *checkpoint, const unsigned char instanceId[32]) {
    DirLease lease;
    char root[65], buf[160];
    uint64_t now;
    if (!l1 || !checkpoint || !instanceId) return NULL;
    now = dirNow();
    dirReadLease(l1, &lease);
    if (!dirLeaseIsMine(&lease, instanceId, now)) return NULL;
    dirHex(checkpoint->postStateRoot, 32, root);
    snprintf(buf, sizeof(buf), "height %llu time %llu root %s\n", (unsigned long long)checkpoint->toHeight, (unsigned long long)now, root);
    if (dirWriteText(l1, "checkpoint", buf) != 0) return NULL;
    if (self->callback) self->callback(self->callbackArg, "CheckpointSubmitted", (void *)checkpoint);
    return dirTx("checkpoint", checkpoint->toHeight);
}

static void dir_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg) {
    if (!l1) return;
    self->callback = callback;
    self->callbackArg = arg;
}

static int dir_poll(HppliteL1 *l1) { return l1 ? 0 : -1; }

static int dir_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs) {
    (void)timeout_secs;
    return (l1 && tx_hash) ? 1 : -1;              /* every write is final when the call returns */
}

/* === Factory: a directory is one rollup === */

static int dir_factory_get_rollup(const char *rpc_url, const char *factory_address, const uint8_t owner_address[20], uint8_t rollup_address_out[20]) {
    (void)rpc_url; (void)factory_address; (void)owner_address;
    memset(rollup_address_out, 0, 20);
    return 0;
}
static int dir_factory_has_rollup(const char *rpc_url, const char *factory_address, const uint8_t owner_address[20]) {
    (void)rpc_url; (void)factory_address; (void)owner_address;
    return 0;
}
static char *dir_factory_get_or_create_rollup(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint8_t rollup_address_out[20]) {
    (void)rpc_url; (void)factory_address; (void)privkey;
    memset(rollup_address_out, 0, 20);
    return NULL;
}
static int dir_factory_list_rollups(const char *rpc_url, const char *factory_address, const uint8_t owner_address[20], uint8_t (*out)[20], int max) {
    (void)rpc_url; (void)factory_address; (void)owner_address; (void)out; (void)max;
    return 0;
}
static int dir_factory_create_rollup(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint64_t checkpointInterval, uint8_t rollup_out[20]) {
    (void)rpc_url; (void)factory_address; (void)privkey; (void)checkpointInterval; (void)rollup_out;
    return 0;
}
static int dir_factory_list_my_rollups(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint8_t (*out)[20], int max) {
    (void)rpc_url; (void)factory_address; (void)privkey; (void)out; (void)max;
    return 0;
}
static int dir_factory_get_my_rollup(const char *rpc_url, const char *factory_address, const uint8_t privkey[32], uint8_t rollup_address_out[20]) {
    (void)rpc_url; (void)factory_address; (void)privkey;
    memset(rollup_address_out, 0, 20);
    return 0;
}

/* === Store === */

static void dirBatchPath(HppliteL1 *l1, uint64_t height, char out[DIR_PATH]) {
    snprintf(out, DIR_PATH, "%s/batch-%llu.bin", self->dir, (unsigned long long)height);
}

static char *dir_submit_batch(HppliteL1 *l1, const uint8_t instanceId[32], uint64_t height, const uint8_t *data, size_t data_len) {
    DirLease lease;
    char path[DIR_PATH];
    if (!l1 || !instanceId || !data || height == 0) return NULL;
    if (data_len > DIR_BATCH_LIMIT) return NULL;
    dirReadLease(l1, &lease);
    if (!dirLeaseIsMine(&lease, instanceId, dirNow())) return NULL;   /* onlyActiveSequencer */
    if (height != dirReadHeight(l1) + 1) return NULL;                 /* non-sequential */
    dirBatchPath(l1, height, path);
    if (dirWrite(path, data, data_len) != 0) return NULL;             /* the batch before the height that announces it */
    if (dirWriteHeight(l1, height) != 0) return NULL;
    return dirTx("batch", height);
}

static uint8_t *dir_get_batch(HppliteL1 *l1, uint64_t height, size_t *data_len_out) {
    char path[DIR_PATH];
    if (data_len_out) *data_len_out = 0;
    if (!l1 || height == 0 || height > dirReadHeight(l1)) return NULL;
    dirBatchPath(l1, height, path);
    return dirRead(path, data_len_out);
}

static int dir_get_batch_hash(HppliteL1 *l1, uint64_t height, uint8_t hash_out[32]) {
    size_t n = 0;
    uint8_t *data = dir_get_batch(l1, height, &n);
    if (!data) { memset(hash_out, 0, 32); return 0; }
    hpplite_keccak256(data, n, hash_out);        /* as HPPLiteDA.submitBatch does */
    free(data);
    return 1;
}

static int dir_get_da_state(HppliteL1 *l1, uint64_t *last_batch_height, uint64_t *total_batches, uint8_t latest_batch_hash[32]) {
    uint64_t h;
    if (!l1) {
        if (last_batch_height) *last_batch_height = 0;
        if (total_batches) *total_batches = 0;
        if (latest_batch_hash) memset(latest_batch_hash, 0, 32);
        return -1;
    }
    h = dirReadHeight(l1);
    if (last_batch_height) *last_batch_height = h;
    if (total_batches) *total_batches = h;
    if (latest_batch_hash) {
        if (h == 0 || dir_get_batch_hash(l1, h, latest_batch_hash) != 1) memset(latest_batch_hash, 0, 32);
    }
    return 0;
}

static int dir_submit_was_oversized(HppliteL1 *l1) { (void)l1; return 0; }
static void dir_set_max_tx_gas(HppliteL1 *l1, uint64_t gas) { (void)l1; (void)gas; }

const HppliteL1Ops hppliteL1DirOps = {
    .name = "dir",
    .connect = dir_connect,
    .connect_config = dir_connect_config,
    .connect_factory = dir_connect_factory,
    .disconnect = dir_disconnect,
    .get_system_config = dir_get_system_config,
    .set_checkpoint_interval = dir_set_checkpoint_interval,
    .set_privkey = dir_set_privkey,
    .get_state = dir_get_state,
    .claim_sequencer = dir_claim_sequencer,
    .renew_lease = dir_renew_lease,
    .release_lease = dir_release_lease,
    .is_lease_active = dir_is_lease_active,
    .get_addresses = dir_get_addresses,
    .get_my_address = dir_get_my_address,
    .get_lease_time_remaining = dir_get_lease_time_remaining,
    .submit_checkpoint = dir_submit_checkpoint,
    .set_callback = dir_set_callback,
    .poll = dir_poll,
    .wait_for_tx = dir_wait_for_tx,
    .factory_get_rollup = dir_factory_get_rollup,
    .factory_has_rollup = dir_factory_has_rollup,
    .factory_get_or_create_rollup = dir_factory_get_or_create_rollup,
    .factory_list_rollups = dir_factory_list_rollups,
    .factory_create_rollup = dir_factory_create_rollup,
    .factory_list_my_rollups = dir_factory_list_my_rollups,
    .factory_get_my_rollup = dir_factory_get_my_rollup,
    .submit_batch = dir_submit_batch,
    .get_batch = dir_get_batch,
    .get_batch_hash = dir_get_batch_hash,
    .get_da_state = dir_get_da_state,
    .submit_was_oversized = dir_submit_was_oversized,
    .set_max_tx_gas = dir_set_max_tx_gas,
};
