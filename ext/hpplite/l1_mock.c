/*
** HPPLite L1 Mock Implementation
**
** In-memory mock for testing singleton sequencer model.
** Implements lease-based sequencer coordination.
*/

#include "l1_backend.h"
#include "crypto.h"
#include "hpplite_sqlite.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

#define MAX_CHECKPOINTS 1000
#define MAX_BATCHES 10000
#define DEFAULT_LEASE_DURATION 300  /* 5 minutes */

/*
** Internal L1 mock structure
*/
typedef struct MockL1 {
    HppliteL1 base;               /* the function table first: l1.c dispatches through it */
    /* Owner */
    unsigned char owner[20];
    unsigned char sequencer[20];     /* The one wallet allowed to claim (the owner by default) */

    /* Sequencer with lease */
    int hasSequencer;
    unsigned char sequencerWallet[20];
    unsigned char sequencerInstance[32];
    uint64_t leaseExpiry;

    /* Configuration */
    uint64_t checkpointInterval;
    uint64_t leaseDuration;

    /* Checkpoints */
    int nCheckpoints;
    HppliteCheckpoint *checkpoints[MAX_CHECKPOINTS];
    uint64_t lastCheckpointTime;
    uint64_t lastCheckpointHeight;
    unsigned char lastStateRoot[32];

    /* Mock L1 block simulation */
    uint64_t currentBlock;
    uint64_t currentTimestamp;

    /* Crypto context for signature verification */
    HppliteCrypto *crypto;

    /* Private key for transactions */
    unsigned char privkey[32];
    int hasPrivkey;
    unsigned char myAddress[20];

    /* Callbacks */
    HppliteL1Callback callback;
    void *callbackArg;

    /* Batch DA storage */
    int failSubmit;              /* Test hook: reject batch submissions */
    int failAfterPlus1;          /* Test hook: 0 = off; else accept this many - 1 more, then reject */
    int loseConfirmation;        /* Test hook: store but report failure */
    int txPending;               /* Test hook: wait_for_tx never confirms */
    int claimReverts;            /* Test hook: a claim is mined and reverts (hash returned, no lease) */
    int rpcDown;                 /* Test hook: every call fails (the endpoint is unreachable) */
    int submitDelayMs;           /* Test hook: wait_for_tx takes this long (a flush in flight) */
    size_t batchSizeLimit;       /* DA size limit (contract default 128 KB) */
    size_t maxTxBytes;           /* Test hook: the RPC's transaction size limit (0 = none) */
    int lastOversized;           /* the last submission failed for that */
    uint64_t nBatches;
    uint8_t *batchData[MAX_BATCHES];
    size_t batchLen[MAX_BATCHES];
    /* a height whose bytes live in a store (#11): the chain's commitment to them.
    ** batchData is NULL there. In memory only: a directory-backed mock forgets them */
    struct MockCommit { uint8_t hash[32]; char *ref; uint8_t *receipt; size_t nReceipt; } *commits[MAX_BATCHES];

    char *dir;                   /* Directory-backed: every change is written here */
    unsigned char address[20];   /* the rollup's identity (from the directory when backed by one) */
} MockL1;

/* The backend's view of the l1 parameter */
#define self ((MockL1 *)l1)

/*
** Directory persistence. A mock that survives the process is what the crash
** harness needs (the writer is kill -9'd, the checker reopens), and it is the
** seed of a chain-free DA backend (issue #35). Layout:
**   <dir>/state          text: block, time, lease, checkpoint summary, batch count
**   <dir>/batch-<N>.bin  raw posted bytes of batch N
**   <dir>/cp-<I>.bin     checkpoint I (binary struct fields)
** Every file is written to a temporary name and renamed into place, so a
** crash leaves either the old or the new content, never a torn file. The
** batch file is written before the state that announces it.
*/
#include <stdio.h>
#include <unistd.h>
static int mockWriteFile(const char *path, const void *data, size_t len) {
    char tmp[1024];
    FILE *f;
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    f = fopen(tmp, "wb");
    if (!f) return -1;
    if (len && fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) { fclose(f); unlink(tmp); return -1; }
    fclose(f);
    return rename(tmp, path);
}
static uint8_t *mockReadFile(const char *path, size_t *pLen) {
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; *pLen = (size_t)n; }
    return buf;
}
static void mockHex(const unsigned char *in, int n, char *out) {
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < n; i++) { out[2*i] = h[in[i] >> 4]; out[2*i+1] = h[in[i] & 15]; }
    out[2*n] = 0;
}
static void mockUnhex(const char *in, unsigned char *out, int n) {
    for (int i = 0; i < n; i++) {
        unsigned v = 0;
        sscanf(in + 2*i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}
static void mockSaveState(HppliteL1 *l1) {
    char path[1024], inst[65], wallet[41], root[65], buf[640];
    if (!self->dir) return;
    mockHex(self->sequencerInstance, 32, inst);
    mockHex(self->sequencerWallet, 20, wallet);
    mockHex(self->lastStateRoot, 32, root);
    snprintf(buf, sizeof(buf), "block %llu\ntime %llu\nbatches %llu\nsequencer %d %s %s %llu\n"
             "checkpoints %d %llu %llu %s\n",
             (unsigned long long)self->currentBlock, (unsigned long long)self->currentTimestamp,
             (unsigned long long)self->nBatches, self->hasSequencer, wallet, inst,
             (unsigned long long)self->leaseExpiry, self->nCheckpoints,
             (unsigned long long)self->lastCheckpointHeight, (unsigned long long)self->lastCheckpointTime, root);
    snprintf(path, sizeof(path), "%s/state", self->dir);
    mockWriteFile(path, buf, strlen(buf));
}
static void mockSaveBatch(HppliteL1 *l1, uint64_t height) {
    char path[1024];
    if (!self->dir) return;
    snprintf(path, sizeof(path), "%s/batch-%llu.bin", self->dir, (unsigned long long)height);
    mockWriteFile(path, self->batchData[height - 1], self->batchLen[height - 1]);
}
static void mockSaveCheckpoint(HppliteL1 *l1, int i) {
    char path[1024];
    if (!self->dir) return;
    snprintf(path, sizeof(path), "%s/cp-%d.bin", self->dir, i);
    mockWriteFile(path, self->checkpoints[i], sizeof(HppliteCheckpoint));
}
static void mockLoad(HppliteL1 *l1) {
    char path[1024], inst[65] = {0}, wallet[41] = {0}, root[65] = {0};
    size_t len = 0;
    unsigned long long block = 0, tm = 0, nb = 0, exp = 0, cph = 0, cpt = 0;
    int hasSeq = 0, ncp = 0;
    uint8_t *txt;
    snprintf(path, sizeof(path), "%s/state", self->dir);
    txt = mockReadFile(path, &len);
    if (!txt) return;                                   /* fresh directory */
    if (sscanf((char *)txt, "block %llu\ntime %llu\nbatches %llu\nsequencer %d %40s %64s %llu\n"
               "checkpoints %d %llu %llu %64s",
               &block, &tm, &nb, &hasSeq, wallet, inst, &exp, &ncp, &cph, &cpt, root) >= 10) {
        self->currentBlock = block;
        self->currentTimestamp = tm;
        self->hasSequencer = hasSeq;
        mockUnhex(wallet, self->sequencerWallet, 20);
        mockUnhex(inst, self->sequencerInstance, 32);
        self->leaseExpiry = exp;
        self->lastCheckpointHeight = cph;
        self->lastCheckpointTime = cpt;
        mockUnhex(root, self->lastStateRoot, 32);
        for (unsigned long long h = 1; h <= nb && h <= MAX_BATCHES; h++) {
            snprintf(path, sizeof(path), "%s/batch-%llu.bin", self->dir, h);
            self->batchData[h - 1] = mockReadFile(path, &self->batchLen[h - 1]);
            if (!self->batchData[h - 1]) break;           /* announced but missing: history ends here */
            self->nBatches = h;
        }
        for (int i = 0; i < ncp && i < MAX_CHECKPOINTS; i++) {
            size_t n = 0;
            snprintf(path, sizeof(path), "%s/cp-%d.bin", self->dir, i);
            uint8_t *raw = mockReadFile(path, &n);
            if (!raw || n != sizeof(HppliteCheckpoint)) { free(raw); break; }
            HppliteCheckpoint *cp = hpplite_checkpoint_new(0, 0);
            if (!cp) { free(raw); break; }
            memcpy(cp, raw, sizeof(HppliteCheckpoint));
            free(raw);
            self->checkpoints[self->nCheckpoints++] = cp;
        }
    }
    free(txt);
}

/*
** Create a new mock L1
*/
static void mockSetAddress(HppliteL1 *l1, const char *seed, uint64_t salt);

HppliteL1 *hpplite_l1_create_mock(void) {
    HppliteL1 *l1 = sqlite3_malloc(sizeof(MockL1));
    if (!l1) return NULL;

    memset(l1, 0, sizeof(MockL1));
    self->base.ops = &hppliteL1MockOps;

    /* Initialize crypto for signature verification */
    self->crypto = hpplite_crypto_init();
    if (!self->crypto) {
        sqlite3_free(l1);
        return NULL;
    }

    /* Default configuration */
    self->checkpointInterval = 100;
    self->leaseDuration = DEFAULT_LEASE_DURATION;

    {
        static uint64_t created;
        mockSetAddress(l1, NULL, (uint64_t)time(NULL) * 1000003u + (uint64_t)(uintptr_t)l1 + ++created);
    }
    /* Initialize mock block state */
    self->currentBlock = 1;
    self->currentTimestamp = (uint64_t)time(NULL);
    self->lastCheckpointTime = self->currentTimestamp;

    return l1;
}

HppliteL1 *hpplite_l1_create_mock_dir(const char *dir) {
    HppliteL1 *l1 = hpplite_l1_create_mock();
    if (!l1 || !dir) return l1;
    self->dir = strdup(dir);
    mockSetAddress(l1, dir, 0);
    mockLoad(l1);
    return l1;
}

/*
** Connect to L1 (for mock, just creates a new instance)
*/
static HppliteL1 *mock_connect(const char *endpoint, const char *contractAddress) {
    (void)endpoint;
    (void)contractAddress;
    return hpplite_l1_create_mock();
}

/*
** Connect from network config
*/
static HppliteL1 *mock_connect_config(const HppliteNetworkConfig *config) {
    (void)config;
    return hpplite_l1_create_mock();
}

/*
** Set private key for transactions
*/
static int mock_set_privkey(HppliteL1 *l1, const uint8_t privkey[32]) {
    if (!l1 || !privkey) return -1;

    memcpy(self->privkey, privkey, 32);
    self->hasPrivkey = 1;

    /* Derive address from private key */
    HppliteKeypair kp;
    int rc = hpplite_crypto_keypair_from_privkey(self->crypto, &kp, privkey);
    if (rc == 0) {
        memcpy(self->myAddress, kp.address, 20);
        /* the factory hands the rollup to the wallet that created it */
        unsigned char zero[20] = {0};
        if (memcmp(self->owner, zero, 20) == 0) memcpy(self->owner, kp.address, 20);
        if (memcmp(self->sequencer, zero, 20) == 0) memcpy(self->sequencer, kp.address, 20);
    }

    return 0;
}

/*
** Disconnect from L1
*/
static void mock_disconnect(HppliteL1 *l1) {
    if (!l1) return;

    /* Free checkpoints */
    for (int i = 0; i < self->nCheckpoints; i++) {
        hpplite_checkpoint_free(self->checkpoints[i]);
    }

    /* Free batch data */
    for (uint64_t i = 0; i < self->nBatches; i++) {
        if (self->batchData[i]) {
            free(self->batchData[i]);
        }
        if (self->commits[i]) {
            sqlite3_free(self->commits[i]->ref);
            sqlite3_free(self->commits[i]->receipt);
            free(self->commits[i]);
        }
    }

    /* Free crypto */
    if (self->crypto) {
        hpplite_crypto_free(self->crypto);
    }

    free(self->dir);
    sqlite3_free(l1);
}

/*
** Get system configuration
*/
static HppliteSystemConfig *mock_get_system_config(HppliteL1 *l1) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1) return NULL;

    HppliteSystemConfig *config = sqlite3_malloc(sizeof(HppliteSystemConfig));
    if (!config) return NULL;

    memset(config, 0, sizeof(HppliteSystemConfig));
    config->daScheme = sqlite3_mprintf("hppda");
    config->version = 4;
    config->checkpointInterval = self->checkpointInterval;
    config->leaseDuration = self->leaseDuration;
    config->batchSizeLimit = self->batchSizeLimit ? self->batchSizeLimit : 128 * 1024;

    return config;
}

/*
** Free system config
*/

/*
** Get current L1 state
*/
static HppliteL1State *mock_get_state(HppliteL1 *l1) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1) return NULL;

    HppliteL1State *state = sqlite3_malloc(sizeof(HppliteL1State));
    if (!state) return NULL;

    memset(state, 0, sizeof(HppliteL1State));

    /* Copy owner */
    memcpy(state->owner, self->owner, 20);

    /* Copy sequencer info */
    if (self->hasSequencer) {
        memcpy(state->sequencerWallet, self->sequencerWallet, 20);
        memcpy(state->sequencerInstance, self->sequencerInstance, 32);
        state->leaseExpiry = self->leaseExpiry;
    }

    /* Copy config */
    state->checkpointInterval = self->checkpointInterval;
    state->leaseDuration = self->leaseDuration;

    /* Copy checkpoint info */
    state->lastCheckpointHeight = self->lastCheckpointHeight;
    memcpy(state->lastStateRoot, self->lastStateRoot, 32);
    state->lastCheckpointTime = self->lastCheckpointTime;

    state->currentBlock = self->currentBlock;
    state->currentTimestamp = self->currentTimestamp;

    return state;
}

/*
** Free L1 state
*/

/* === Lease Management === */

/*
** Check if lease is currently active
*/

static int mock_get_addresses(HppliteL1 *l1, char contractHex[43], char daHex[43]) {
    static const char *h = "0123456789abcdef";
    int i;
    if (!l1) return -1;
    contractHex[0] = '0'; contractHex[1] = 'x';
    for (i = 0; i < 20; i++) {
        contractHex[2 + 2*i] = h[self->address[i] >> 4];
        contractHex[3 + 2*i] = h[self->address[i] & 15];
    }
    contractHex[42] = 0;
    daHex[0] = 0;
    return 0;
}

/* each mock is a rollup of its own; one backed by a directory is the same
** rollup every time it is loaded from there */
static void mockSetAddress(HppliteL1 *l1, const char *seed, uint64_t salt) {
    uint64_t x = 1469598103934665603ULL ^ salt;
    int i;
    for (; seed && *seed; seed++) x = (x ^ (unsigned char)*seed) * 1099511628211ULL;
    for (i = 0; i < 20; i++) { x = (x ^ (uint64_t)i) * 1099511628211ULL; self->address[i] = (unsigned char)(x >> 32); }
}

static int mock_get_my_address(HppliteL1 *l1, unsigned char out[20]) {
    if (!l1 || !self->hasPrivkey) return -1;
    memcpy(out, self->myAddress, 20);
    return 0;
}

void hpplite_l1_mock_claim_foreign(HppliteL1 *l1, const unsigned char *wallet,
                                   const unsigned char instanceId[32], uint64_t seconds) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    self->hasSequencer = 1;
    if (wallet) memcpy(self->sequencerWallet, wallet, 20);
    else memset(self->sequencerWallet, 0x55, 20);          /* nobody we know */
    memcpy(self->sequencerInstance, instanceId, 32);
    self->leaseExpiry = self->currentTimestamp + seconds;
    mockSaveState(l1);
}

void hpplite_l1_mock_inject_batch(HppliteL1 *l1, const uint8_t *data, size_t len) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (!l1 || !data || self->nBatches >= MAX_BATCHES) return;
    size_t idx = self->nBatches;
    self->batchData[idx] = malloc(len);
    if (!self->batchData[idx]) return;
    memcpy(self->batchData[idx], data, len);
    self->batchLen[idx] = len;
    self->nBatches++;
    mockSaveBatch(l1, self->nBatches);
    mockSaveState(l1);
}

static int mock_is_lease_active(HppliteL1 *l1) {
    if (!l1 || !self->hasSequencer) return 0;
    return self->currentTimestamp < self->leaseExpiry;
}

/*
** Release the lease (owner expires it)
*/
static char *mock_release_lease(HppliteL1 *l1, const unsigned char instanceId[32]) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1 || !self->hasPrivkey || !self->hasSequencer) return NULL;
    if (!instanceId) {
        /* forceExpireLease(): onlyOwner */
        if (memcmp(self->owner, self->myAddress, 20) != 0) return NULL;
    } else {
        /* releaseLease(instanceId): the holder's wallet and instance, while active */
        if (memcmp(self->sequencerWallet, self->myAddress, 20) != 0) return NULL;   /* "Not sequencer wallet" */
        if (memcmp(self->sequencerInstance, instanceId, 32) != 0) return NULL;   /* "Wrong instance" */
        if (!mock_is_lease_active(l1)) return NULL;                       /* "Lease expired" */
    }
    self->leaseExpiry = self->currentTimestamp;
    mockSaveState(l1);
    return sqlite3_mprintf("0x%08x%08x",
        (unsigned int)(self->currentBlock),
        (unsigned int)(self->currentTimestamp));
}

/*
** Get remaining lease time
*/
static uint64_t mock_get_lease_time_remaining(HppliteL1 *l1) {
    if (!l1 || !self->hasSequencer) return 0;
    if (self->currentTimestamp >= self->leaseExpiry) return 0;
    return self->leaseExpiry - self->currentTimestamp;
}


/*
** Claim sequencer lease with instance ID
*/
static char *mock_claim_sequencer(
    HppliteL1 *l1,
    const unsigned char instanceId[32]
) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1 || !instanceId) return NULL;
    if (!self->hasPrivkey) return NULL;
    if (memcmp(self->sequencer, self->myAddress, 20) != 0) return NULL;   /* "Not the sequencer" */

    /* Check if lease is available */
    if (self->hasSequencer && mock_is_lease_active(l1)) {
        return NULL;  /* Lease still active */
    }
    if (self->claimReverts) {
        /* mined, then reverted: the caller gets a hash, the chain no lease */
        return sqlite3_mprintf("0xreverted%08x", (unsigned int)(self->currentTimestamp));
    }

    /* Claim the lease */
    self->hasSequencer = 1;
    memcpy(self->sequencerWallet, self->myAddress, 20);
    memcpy(self->sequencerInstance, instanceId, 32);
    self->leaseExpiry = self->currentTimestamp + self->leaseDuration;
    mockSaveState(l1);

    /* Generate mock transaction hash */
    char *txHash = sqlite3_mprintf("0x%08x%08x",
        (unsigned int)(self->currentBlock),
        (unsigned int)(self->currentTimestamp));

    /* Emit event */
    if (self->callback) {
        self->callback(self->callbackArg, "SequencerClaimed", (void *)instanceId);
    }

    return txHash;
}

/*
** Renew sequencer lease
*/
static char *mock_renew_lease(
    HppliteL1 *l1,
    const unsigned char instanceId[32]
) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1 || !instanceId) return NULL;
    if (!self->hasPrivkey) return NULL;

    /* Must be current sequencer */
    if (!self->hasSequencer) return NULL;
    if (memcmp(self->sequencerWallet, self->myAddress, 20) != 0) return NULL;
    if (memcmp(self->sequencerInstance, instanceId, 32) != 0) return NULL;

    /* Lease must not be expired */
    if (!mock_is_lease_active(l1)) return NULL;

    /* Renew */
    self->leaseExpiry = self->currentTimestamp + self->leaseDuration;
    mockSaveState(l1);

    /* Generate mock transaction hash */
    char *txHash = sqlite3_mprintf("0x%08xrenew%08x",
        (unsigned int)(self->currentBlock),
        (unsigned int)(self->currentTimestamp));

    return txHash;
}

/* === Checkpoint Operations === */

/*
** Submit a checkpoint to L1
*/
static char *mock_submit_checkpoint(
    HppliteL1 *l1,
    const HppliteCheckpoint *checkpoint,
    const unsigned char instanceId[32]
) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1 || !checkpoint || !instanceId) return NULL;

    /* onlyActiveSequencer(instanceId): the holder's wallet and instance, while active */
    if (!self->hasPrivkey || memcmp(self->sequencerWallet, self->myAddress, 20) != 0) {
        return NULL;  /* "Not sequencer wallet" */
    }
    if (memcmp(instanceId, self->sequencerInstance, 32) != 0) {
        return NULL;  /* Wrong instance */
    }
    if (self->leaseExpiry <= self->currentTimestamp) {
        return NULL;  /* Lease expired */
    }

    /* Store checkpoint */
    if (self->nCheckpoints >= MAX_CHECKPOINTS) {
        return NULL;
    }

    HppliteCheckpoint *cpCopy = hpplite_checkpoint_new(checkpoint->fromHeight, checkpoint->toHeight);
    if (!cpCopy) return NULL;

    memcpy(cpCopy->preStateRoot, checkpoint->preStateRoot, 32);
    memcpy(cpCopy->postStateRoot, checkpoint->postStateRoot, 32);
    memcpy(cpCopy->batchesHash, checkpoint->batchesHash, 32);
    cpCopy->timestamp = checkpoint->timestamp;

    self->checkpoints[self->nCheckpoints++] = cpCopy;
    self->lastCheckpointTime = self->currentTimestamp;
    self->lastCheckpointHeight = checkpoint->toHeight;
    memcpy(self->lastStateRoot, checkpoint->postStateRoot, 32);
    mockSaveCheckpoint(l1, self->nCheckpoints - 1);
    mockSaveState(l1);

    /* Auto-renew lease on checkpoint */
    if (self->hasSequencer && mock_is_lease_active(l1)) {
        self->leaseExpiry = self->currentTimestamp + self->leaseDuration;
        mockSaveState(l1);
    }

    /* Generate mock transaction hash */
    char *txHash = sqlite3_mprintf("0xcp%08x%08x",
        (unsigned int)(self->currentBlock),
        (unsigned int)(self->nCheckpoints));

    /* Emit event */
    if (self->callback) {
        self->callback(self->callbackArg, "CheckpointSubmitted", cpCopy);
    }

    return txHash;
}

/*
** Set callback for L1 events
*/
static void mock_set_callback(HppliteL1 *l1, HppliteL1Callback callback, void *arg) {
    if (!l1) return;
    self->callback = callback;
    self->callbackArg = arg;
}

/*
** Poll for L1 updates (mock: advance block)
*/
static int mock_poll(HppliteL1 *l1) {
    if (!l1) return -1;

    /* Simulate block advancement (12 seconds per block) */
    self->currentBlock++;
    self->currentTimestamp += 12;

    return 0;
}

static int mock_wait_for_tx(HppliteL1 *l1, const char *tx_hash, int timeout_secs) {
    (void)timeout_secs;
    if (l1 && self->txPending) return -1;   /* still pending after the timeout */
    if (tx_hash && strncmp(tx_hash, "0xreverted", 10) == 0) return 0;   /* mined, status 0 */
    if (l1 && self->submitDelayMs > 0) usleep((useconds_t)self->submitDelayMs * 1000);
    return 1;
}

/* === Mock-specific functions for testing === */

void hpplite_l1_mock_set_submit_delay_ms(HppliteL1 *l1, int ms) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->submitDelayMs = ms;
}

static char *mock_set_checkpoint_interval(HppliteL1 *l1, uint64_t interval) {
    if (!l1 || !self->hasPrivkey || interval == 0) return NULL;
    if (memcmp(self->owner, self->myAddress, 20) != 0) return NULL;   /* onlyOwner */
    self->checkpointInterval = interval;
    mockSaveState(l1);
    return sqlite3_mprintf("0x%08xckpt%08x", (unsigned int)self->currentBlock, (unsigned int)interval);
}

void hpplite_l1_mock_set_rpc_down(HppliteL1 *l1, int down) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->rpcDown = down;
}

void hpplite_l1_mock_set_claim_reverts(HppliteL1 *l1, int revert) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->claimReverts = revert;
}

void hpplite_l1_mock_set_fail_submit(HppliteL1 *l1, int fail) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) { self->failSubmit = fail; self->failAfterPlus1 = 0; }
}

void hpplite_l1_mock_truncate(HppliteL1 *l1, uint64_t keep) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (!l1) return;
    while (self->nBatches > keep) {
        self->nBatches--;
        free(self->batchData[self->nBatches]);
        self->batchData[self->nBatches] = NULL;
        self->batchLen[self->nBatches] = 0;
    }
    mockSaveState(l1);
}

void hpplite_l1_mock_set_max_tx_bytes(HppliteL1 *l1, size_t n) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->maxTxBytes = n;
}

static int mock_submit_was_oversized(HppliteL1 *l1) {
    return l1 ? self->lastOversized : 0;
}

static void mock_set_max_tx_gas(HppliteL1 *l1, uint64_t gas) {
    (void)l1; (void)gas;                 /* the mock charges no gas */
}

void hpplite_l1_mock_set_fail_submit_after(HppliteL1 *l1, int n) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->failAfterPlus1 = n + 1;
}

void hpplite_l1_mock_set_lose_confirmation(HppliteL1 *l1, int lose) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->loseConfirmation = lose;
}

void hpplite_l1_mock_set_tx_pending(HppliteL1 *l1, int pending) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->txPending = pending;
}

void hpplite_l1_mock_set_batch_size_limit(HppliteL1 *l1, size_t limit) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (l1) self->batchSizeLimit = limit;
}

void hpplite_l1_mock_set_sequencer(HppliteL1 *l1, const unsigned char *pubkey) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (!l1 || !pubkey) return;

    self->hasSequencer = 1;
    /* Just use pubkey bytes as address for mock */
    memcpy(self->sequencerWallet, pubkey, 20);
    memset(self->sequencerInstance, 0, 32);
    self->leaseExpiry = self->currentTimestamp + self->leaseDuration;
    mockSaveState(l1);
}

void hpplite_l1_mock_advance_blocks(HppliteL1 *l1, uint64_t blocks) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (!l1) return;
    self->currentBlock += blocks;
    self->currentTimestamp += blocks * 12;
}

void hpplite_l1_mock_advance_time(HppliteL1 *l1, uint64_t seconds) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (!l1) return;
    self->currentTimestamp += seconds;
    self->currentBlock += seconds / 12;
}

void hpplite_l1_mock_set_config(
    HppliteL1 *l1,
    uint64_t checkpointInterval,
    uint64_t leaseDuration
) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return;
    if (!l1) return;
    self->checkpointInterval = checkpointInterval;
    self->leaseDuration = leaseDuration;
}

uint64_t hpplite_l1_mock_get_timestamp(HppliteL1 *l1) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return 0;
    return l1 ? self->currentTimestamp : 0;
}

uint64_t hpplite_l1_mock_get_block(HppliteL1 *l1) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return 0;
    return l1 ? self->currentBlock : 0;
}

const HppliteCheckpoint *hpplite_l1_mock_get_last_checkpoint(HppliteL1 *l1) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return NULL;
    if (!l1 || self->nCheckpoints == 0) return NULL;
    return self->checkpoints[self->nCheckpoints - 1];
}

int hpplite_l1_mock_get_checkpoint_count(HppliteL1 *l1) {
    if (!l1 || l1->ops != &hppliteL1MockOps) return 0;
    return l1 ? self->nCheckpoints : 0;
}

/* === Factory Functions (stubs for mock) === */

static int mock_factory_get_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t rollup_address_out[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)owner_address;
    memset(rollup_address_out, 0, 20);
    return 0;
}

static int mock_factory_has_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)owner_address;
    return 0;
}

static char *mock_factory_get_or_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)privkey;
    memset(rollup_address_out, 0, 20);
    return NULL;
}

static int mock_factory_get_my_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t rollup_address_out[20]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)privkey;
    memset(rollup_address_out, 0, 20);
    return 0;
}

static int mock_factory_list_rollups(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t owner_address[20],
    uint8_t (*out)[20],
    int max
) {
    (void)rpc_url; (void)factory_address; (void)owner_address; (void)out; (void)max;
    return 0;
}

static int mock_factory_create_rollup(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint64_t checkpointInterval,
    uint8_t rollup_out[20]
) {
    (void)rpc_url; (void)factory_address; (void)privkey; (void)checkpointInterval; (void)rollup_out;
    return 0;
}

static int mock_factory_list_my_rollups(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32],
    uint8_t (*out)[20],
    int max
) {
    (void)rpc_url; (void)factory_address; (void)privkey; (void)out; (void)max;
    return 0;
}

static HppliteL1 *mock_connect_factory(
    const char *rpc_url,
    const char *factory_address,
    const uint8_t privkey[32]
) {
    (void)rpc_url;
    (void)factory_address;
    (void)privkey;
    return NULL;
}

/* === Batch DA Functions === */

static char *mock_submit_batch(
    HppliteL1 *l1,
    const uint8_t instanceId[32],
    uint64_t height,
    const uint8_t *data,
    size_t data_len
) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1 || !instanceId || !data || height == 0) return NULL;
    if (height > MAX_BATCHES) return NULL;
    self->lastOversized = 0;
    if (self->failSubmit) return NULL;
    if (self->failAfterPlus1 == 1) return NULL;
    if (self->maxTxBytes && data_len > self->maxTxBytes) { self->lastOversized = 1; return NULL; }
    if (data_len > (self->batchSizeLimit ? self->batchSizeLimit : 128 * 1024)) {
        return NULL;  /* "Batch too large" */
    }
    if (height != self->nBatches + 1) {
        return NULL;  /* "Non-sequential batch" */
    }

    /* onlyActiveSequencer(instanceId): the holder's wallet and instance, while active */
    if (!self->hasPrivkey || memcmp(self->sequencerWallet, self->myAddress, 20) != 0) {
        return NULL;  /* "Not sequencer wallet" */
    }
    if (memcmp(instanceId, self->sequencerInstance, 32) != 0) {
        return NULL;  /* Wrong instance */
    }
    if (self->leaseExpiry <= self->currentTimestamp) {
        return NULL;  /* Lease expired */
    }

    size_t idx = height - 1;

    /* Free existing data if overwriting */
    if (self->batchData[idx]) {
        free(self->batchData[idx]);
    }

    self->batchData[idx] = malloc(data_len);
    if (!self->batchData[idx]) return NULL;

    memcpy(self->batchData[idx], data, data_len);
    self->batchLen[idx] = data_len;
    if (self->failAfterPlus1 > 1) self->failAfterPlus1--;

    if (height > self->nBatches) {
        self->nBatches = height;
    }
    mockSaveBatch(l1, height);
    mockSaveState(l1);

    if (self->loseConfirmation) return NULL;  /* stored, but the caller never hears */
    return sqlite3_mprintf("0x%016llx", (unsigned long long)height);
}

static uint8_t *mock_get_batch(
    HppliteL1 *l1,
    uint64_t height,
    size_t *data_len_out
) {
    if (l1 && self->rpcDown) return NULL;   /* the endpoint is down */
    if (!l1 || height == 0 || height > MAX_BATCHES) {
        if (data_len_out) *data_len_out = 0;
        return NULL;
    }

    size_t idx = height - 1;
    if (!self->batchData[idx]) {
        if (data_len_out) *data_len_out = 0;
        return NULL;
    }

    uint8_t *copy = malloc(self->batchLen[idx] + 1);     /* terminated, as the chain's copy is (#77) */
    if (!copy) {
        if (data_len_out) *data_len_out = 0;
        return NULL;
    }

    memcpy(copy, self->batchData[idx], self->batchLen[idx]);
    copy[self->batchLen[idx]] = 0;
    if (data_len_out) *data_len_out = self->batchLen[idx];
    return copy;
}

static int mock_get_batch_hash(
    HppliteL1 *l1,
    uint64_t height,
    uint8_t hash_out[32]
) {
    if (!l1 || height == 0 || height > MAX_BATCHES) {
        memset(hash_out, 0, 32);
        return 0;
    }

    size_t idx = height - 1;
    if (!self->batchData[idx]) {
        if (self->commits[idx]) { memcpy(hash_out, self->commits[idx]->hash, 32); return 1; }   /* committed, bytes in a store (#11) */
        memset(hash_out, 0, 32);
        return 0;
    }

    hpplite_keccak256(self->batchData[idx], self->batchLen[idx], hash_out);   /* as HPPLiteDA.submitBatch does */
    return 1;
}

/* commitBatch (#11): the same gate as submitBatch, then hash, reference and receipt at the height */
static char *mock_commit_batch(HppliteL1 *l1, const uint8_t instanceId[32], uint64_t height, const uint8_t hash[32],
                               const char *zRef, const uint8_t *receipt, size_t nReceipt) {
    struct MockCommit *c;
    size_t idx;
    if (l1 && self->rpcDown) return NULL;
    if (!l1 || !instanceId || !hash || !zRef || !zRef[0] || height == 0 || height > MAX_BATCHES) return NULL;
    if (self->failSubmit || self->failAfterPlus1 == 1) return NULL;
    if (height != self->nBatches + 1) return NULL;
    if (!self->hasPrivkey || memcmp(self->sequencerWallet, self->myAddress, 20) != 0) return NULL;
    if (memcmp(instanceId, self->sequencerInstance, 32) != 0) return NULL;
    if (self->leaseExpiry <= self->currentTimestamp) return NULL;
    c = calloc(1, sizeof *c);
    if (!c) return NULL;
    memcpy(c->hash, hash, 32);
    c->ref = sqlite3_mprintf("%s", zRef);
    if (receipt && nReceipt) {
        c->receipt = sqlite3_malloc((int)nReceipt);
        if (c->receipt) { memcpy(c->receipt, receipt, nReceipt); c->nReceipt = nReceipt; }
    }
    idx = height - 1;
    if (self->commits[idx]) { sqlite3_free(self->commits[idx]->ref); sqlite3_free(self->commits[idx]->receipt); free(self->commits[idx]); }
    self->commits[idx] = c;
    free(self->batchData[idx]);
    self->batchData[idx] = NULL;
    self->batchLen[idx] = 0;
    if (self->failAfterPlus1 > 1) self->failAfterPlus1--;
    if (height > self->nBatches) self->nBatches = height;
    mockSaveState(l1);
    if (self->loseConfirmation) return NULL;
    return sqlite3_mprintf("0x%016llx", (unsigned long long)height);
}

static int mock_get_commitment(HppliteL1 *l1, uint64_t height, uint8_t hash[32], char **pzRef, uint8_t **pReceipt, size_t *pnReceipt) {
    struct MockCommit *c;
    if (pzRef) *pzRef = NULL;
    if (pReceipt) *pReceipt = NULL;
    if (pnReceipt) *pnReceipt = 0;
    if (!l1 || self->rpcDown || height == 0 || height > MAX_BATCHES) return -1;
    c = self->commits[height - 1];
    if (!c) return -1;
    if (hash) memcpy(hash, c->hash, 32);
    if (pzRef) *pzRef = sqlite3_mprintf("%s", c->ref);
    if (pReceipt && c->receipt) {
        *pReceipt = sqlite3_malloc((int)c->nReceipt);
        if (*pReceipt) { memcpy(*pReceipt, c->receipt, c->nReceipt); if (pnReceipt) *pnReceipt = c->nReceipt; }
    }
    return 0;
}

static int mock_get_da_state(
    HppliteL1 *l1,
    uint64_t *last_batch_height,
    uint64_t *total_batches,
    uint8_t latest_batch_hash[32]
) {
    if (l1 && self->rpcDown) return -1;   /* the endpoint is down */
    if (!l1) {
        if (last_batch_height) *last_batch_height = 0;
        if (total_batches) *total_batches = 0;
        if (latest_batch_hash) memset(latest_batch_hash, 0, 32);
        return -1;
    }

    if (last_batch_height) *last_batch_height = self->nBatches;
    if (total_batches) *total_batches = self->nBatches;

    if (latest_batch_hash) {
        if (self->nBatches > 0) {
            mock_get_batch_hash(l1, self->nBatches, latest_batch_hash);
        } else {
            memset(latest_batch_hash, 0, 32);
        }
    }
    return 0;
}

static int mock_get_chain_hash(HppliteL1 *l1, uint64_t *pHeight, uint8_t out[32]) {
    (void)l1; (void)pHeight; (void)out;
    return -1;                                    /* the mock keeps no rolling hash (#33) */
}

const HppliteL1Ops hppliteL1MockOps = {
    .name = "mock",
    .connect = mock_connect,
    .connect_config = mock_connect_config,
    .connect_factory = mock_connect_factory,
    .disconnect = mock_disconnect,
    .get_system_config = mock_get_system_config,
    .set_checkpoint_interval = mock_set_checkpoint_interval,
    .set_privkey = mock_set_privkey,
    .get_state = mock_get_state,
    .claim_sequencer = mock_claim_sequencer,
    .renew_lease = mock_renew_lease,
    .release_lease = mock_release_lease,
    .is_lease_active = mock_is_lease_active,
    .get_addresses = mock_get_addresses,
    .get_my_address = mock_get_my_address,
    .get_lease_time_remaining = mock_get_lease_time_remaining,
    .submit_checkpoint = mock_submit_checkpoint,
    .set_callback = mock_set_callback,
    .poll = mock_poll,
    .wait_for_tx = mock_wait_for_tx,
    .factory_get_rollup = mock_factory_get_rollup,
    .factory_has_rollup = mock_factory_has_rollup,
    .factory_get_or_create_rollup = mock_factory_get_or_create_rollup,
    .factory_list_rollups = mock_factory_list_rollups,
    .factory_create_rollup = mock_factory_create_rollup,
    .factory_list_my_rollups = mock_factory_list_my_rollups,
    .factory_get_my_rollup = mock_factory_get_my_rollup,
    .submit_batch = mock_submit_batch,
    .get_batch = mock_get_batch,
    .get_batch_hash = mock_get_batch_hash,
    .get_da_state = mock_get_da_state,
    .submit_was_oversized = mock_submit_was_oversized,
    .set_max_tx_gas = mock_set_max_tx_gas,
    .get_chain_hash = mock_get_chain_hash,
    .commit_batch = mock_commit_batch,
    .get_commitment = mock_get_commitment,
};
