/*
** hpplite-verify: rebuild a database from the chain alone and check it.
**
**   HPPLITE_PARAMS="l1=hpp-sepolia&keyfile=/path/to/key" \
**     hpplite-verify [--db /path/to/live.db] [--keep <dir>] [--to <height> | --at <time>]
**     hpplite-verify --list
**
** Opens an empty file with role=verify and the same parameters the
** application uses (l1=/rpc=/factory=/contract=, the key, dbname=): every
** batch is fetched, opened and replayed, and after each batch that carries
** a state root the content hash of the rebuilt database is compared with
** the posted one. The root anchored on the contract by the last checkpoint
** is compared with the rebuilt content at that height. A verifier never
** posts, never claims the lease and never creates a rollup (#5).
**
** --db <file>: also compare the live database with the rebuilt one - the
** same content hash as the state root; when they differ and nothing is
** pending in the live file, the tables that differ are named.
**
** --to <height>: stop after that batch (#206): with --keep, the database as
** of that height stays; the checkpoints passed are checked, and whether the
** stop height carries a root of its own is said.
**
** A history pruned at the store (keep=, #223): batch 1 cannot be read, so
** the rebuild starts from the oldest snapshot the anchors still name (#224),
** its root checked against the one the chain anchored at its height, and
** says "verified from the snapshot at h". --to/--at before it cannot be met.
**
** With a stop (--to/--at) the rebuild begins at the newest snapshot at or
** before the stop (#391), pruned or not, and says so: the state at a point
** is what is asked for. Without a stop every batch from batch 1 is read.
**
** --at <time> (unix seconds, or YYYY-MM-DDTHH:MM:SSZ; either with .mmm): point-in-time recovery
** (#205) - every batch its writer wrote by then, by the time each batch
** carries; a transaction in flight at that moment is completed, never left
** halfway. With --keep, the database as of that time stays.
**
** A stop past the last anchor (#390): the store's batches past it are read up
** to the stop, each signed by the rollup's sequencer and applied on the
** anchored history, and with a stop one line says which part of what was
** rebuilt rests on the chain: "anchored to H, rebuilt to N". --to past the
** store's last batch cannot be met (exit 2); --at after it answers with the
** last batch the store holds. A batch there that is not the sequencer's or
** does not apply stops the rebuild before it (exit 1).
**
** --list (#397): the rollup's batches without a rebuild, a JSON line each on
** stdout, oldest first - what there is to choose a --to or an --at from:
**   {"height":h,"time":"YYYY-MM-DDTHH:MM:SS.mmmZ","time_ms":ms,"bytes":n,"anchored":true}
** (--at takes that time as it is, and goes through that batch; --to its height).
** The anchored ranges' manifests are read, one read a range, each checked
** against the chain (its digest, its batches' fold to the anchored chain
** hash) - and no batch: the time and the bytes are what the sequencer wrote
** in the manifest (since v0.12.0), not compared with the batches by anyone.
** A range whose manifest names no times is one line,
**   {"from":a,"to":b,"batches":n,"anchored":true}
** with "held":false where the store no longer holds the manifest (keep=).
** Past the last anchor the store's batches are read one by one, each signed
** by the rollup's sequencer ("anchored":false; the time is in the batch's
** head, absent when this key does not open it). Nothing is verified: exit 0
** listed to the end, 1 the listing stopped (why on stderr), 2 cannot run.
**
** Exit 0: verified (and the live file equals the chain, with --db);
** 1: a root mismatch, a sync that stopped short, or a live file that
** differs; 2: cannot run (parameters, chain, no rollup).
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE        /* strptime, timegm (--at) */
#endif
#include "hpplite.h"
#include "node.h"
#include "l1_interface.h"
#include "crypto.h"
#include "config.h"
#include "store.h"
#include "da_uri.h"
#include <time.h>
#include <ftw.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int rm_entry(const char *path, const struct stat *sb, int type, struct FTW *ftwbuf) {
    (void)sb; (void)type; (void)ftwbuf;
    return remove(path);
}

static char *status_text(sqlite3 *db, const char *col) {
    static char buf[8][512];
    static int which;
    char *out = buf[which = (which + 1) % 8];
    char sql[96];
    sqlite3_stmt *st;
    snprintf(sql, sizeof(sql), "SELECT %s FROM hpplite_status", col);
    out[0] = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return out;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_type(st, 0) != SQLITE_NULL)
        snprintf(out, 512, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

static sqlite3_int64 scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st;
    sqlite3_int64 v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void hex(const unsigned char *h, char *out) {
    static const char *d = "0123456789abcdef";
    for (int i = 0; i < 32; i++) { out[2 * i] = d[h[i] >> 4]; out[2 * i + 1] = d[h[i] & 15]; }
    out[64] = 0;
}

/* Content hash of one table: every row in rowid (or key) order, typed */
static int table_hash(sqlite3 *db, const char *zTab, unsigned char out[32], sqlite3_int64 *pRows) {
    char *zSql = sqlite3_mprintf("SELECT * FROM main.\"%w\" ORDER BY rowid", zTab);
    sqlite3_stmt *st = NULL;
    size_t cap = 1 << 16, len = 0;
    unsigned char *buf = malloc(cap);
    int rc;
    *pRows = 0;
    if (!zSql || !buf) { sqlite3_free(zSql); free(buf); return SQLITE_NOMEM; }
    rc = sqlite3_prepare_v2(db, zSql, -1, &st, NULL);
    if (rc != SQLITE_OK) {                                   /* WITHOUT ROWID: order by every column */
        sqlite3_free(zSql);
        zSql = sqlite3_mprintf("SELECT * FROM main.\"%w\" ORDER BY 1", zTab);
        rc = zSql ? sqlite3_prepare_v2(db, zSql, -1, &st, NULL) : SQLITE_NOMEM;
    }
    sqlite3_free(zSql);
    if (rc != SQLITE_OK) { free(buf); return rc; }
    while (sqlite3_step(st) == SQLITE_ROW) {
        int n = sqlite3_column_count(st);
        (*pRows)++;
        for (int i = 0; i < n; i++) {
            int t = sqlite3_column_type(st, i);
            const unsigned char *z = t == SQLITE_NULL ? (const unsigned char *)"" : sqlite3_column_blob(st, i);
            int nz = t == SQLITE_NULL ? 0 : sqlite3_column_bytes(st, i);
            while (len + (size_t)nz + 6 > cap) { cap *= 2; buf = realloc(buf, cap); if (!buf) { sqlite3_finalize(st); return SQLITE_NOMEM; } }
            buf[len++] = (unsigned char)t;
            buf[len++] = (unsigned char)(nz >> 24); buf[len++] = (unsigned char)(nz >> 16);
            buf[len++] = (unsigned char)(nz >> 8);  buf[len++] = (unsigned char)nz;
            if (nz) memcpy(buf + len, z, (size_t)nz);
            len += (size_t)nz;
        }
        buf[len++] = 0xff;
    }
    sqlite3_finalize(st);
    hpplite_keccak256(buf, len, out);
    free(buf);
    return SQLITE_OK;
}

/* The tables that differ between two databases: named on stdout, counted */
static int report_table_differences(sqlite3 *a, sqlite3 *b) {
    sqlite3_stmt *st;
    int nDiff = 0;
    const char *zList = "SELECT name FROM sqlite_schema WHERE type = 'table' AND name NOT LIKE 'sqlite\\_%' ESCAPE '\\'"
                        " AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\' ORDER BY name";
    if (sqlite3_prepare_v2(a, zList, -1, &st, NULL) != SQLITE_OK) return -1;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *zTab = (const char *)sqlite3_column_text(st, 0);
        unsigned char ha[32], hb[32];
        sqlite3_int64 ra = 0, rb = 0;
        char *q = sqlite3_mprintf("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = %Q", zTab);
        int inB = q && scalar(b, q) == 1;
        sqlite3_free(q);
        if (!inB) { printf("  table %s: only in the live database\n", zTab); nDiff++; continue; }
        if (table_hash(a, zTab, ha, &ra) != SQLITE_OK || table_hash(b, zTab, hb, &rb) != SQLITE_OK) continue;
        if (memcmp(ha, hb, 32) != 0) {
            printf("  table %s: differs (%lld rows live, %lld rebuilt)\n", zTab, (long long)ra, (long long)rb);
            nDiff++;
        }
    }
    sqlite3_finalize(st);
    /* tables the chain has and the live file does not */
    if (sqlite3_prepare_v2(b, zList, -1, &st, NULL) == SQLITE_OK) {
        while (sqlite3_step(st) == SQLITE_ROW) {
            const char *zTab = (const char *)sqlite3_column_text(st, 0);
            char *q = sqlite3_mprintf("SELECT count(*) FROM sqlite_schema WHERE type = 'table' AND name = %Q", zTab);
            if (q && scalar(a, q) != 1) { printf("  table %s: only in the rebuilt database\n", zTab); nDiff++; }
            sqlite3_free(q);
        }
        sqlite3_finalize(st);
    }
    return nDiff;
}

/* --at's time: unix seconds, or YYYY-MM-DDTHH:MM:SS(Z), in UTC; ms since the epoch, 0 when it is neither */
static uint64_t parse_time_ms(const char *z) {
    struct tm tm;
    const char *end;
    uint64_t ms;
    size_t d;
    int iso = 0;
    if (!z || !z[0]) return 0;
    d = strspn(z, "0123456789");
    if (d && (!z[d] || z[d] == '.')) { ms = (uint64_t)atoll(z) * 1000; end = z + d; }
    else {
        memset(&tm, 0, sizeof tm);
        end = strptime(z, "%Y-%m-%dT%H:%M:%S", &tm);
        if (!end) return 0;
        ms = (uint64_t)timegm(&tm) * 1000;
        iso = 1;
    }
    if (*end == '.') {
        /* a fraction, to the millisecond: the time --list prints for a batch (#397) */
        unsigned frac = 0;
        int k;
        for (k = 0, end++; k < 3 && *end >= '0' && *end <= '9'; k++, end++) frac = frac * 10 + (unsigned)(*end - '0');
        if (k == 0) return 0;
        for (; k < 3; k++) frac *= 10;
        ms += frac;
    }
    if (*end && !(iso && strcmp(end, "Z") == 0)) return 0;
    return ms;
}

static void fmt_time(uint64_t ms, char *out, size_t n) {
    time_t t = (time_t)(ms / 1000);
    strftime(out, n, "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
}

static int usage(const char *argv0) {
    fprintf(stderr, "usage: HPPLITE_PARAMS=\"l1=...&keyfile=...\" %s [--db <live database>] [--keep <dir>] [--to <height> | --at <time>]\n"
                    "       HPPLITE_PARAMS=\"l1=...&keyfile=...\" %s --list\n", argv0, argv0);
    return 2;
}

/* --list (#397): a batch, or a range listed whole, as a JSON line */
static int list_line(void *arg, const HppliteListed *b) {
    (void)arg;
    if (b->from == b->to) printf("{\"height\":%llu", (unsigned long long)b->from);
    else printf("{\"from\":%llu,\"to\":%llu,\"batches\":%llu", (unsigned long long)b->from, (unsigned long long)b->to, (unsigned long long)(b->to - b->from + 1));
    if (b->timeMs) {
        char z[40];
        time_t t = (time_t)(b->timeMs / 1000);
        strftime(z, sizeof z, "%Y-%m-%dT%H:%M:%S", gmtime(&t));
        /* to the millisecond, as --at takes it: a rebuild --at this time goes through this batch */
        printf(",\"time\":\"%s.%03uZ\",\"time_ms\":%llu", z, (unsigned)(b->timeMs % 1000), (unsigned long long)b->timeMs);
    }
    if (b->bytes) printf(",\"bytes\":%llu", (unsigned long long)b->bytes);
    printf(",\"anchored\":%s%s}\n", b->anchored ? "true" : "false", b->held ? "" : ",\"held\":false");
    return ferror(stdout) ? 1 : 0;
}

int main(int argc, char **argv) {
    uint64_t stopAt = 0;            /* --to (#206) */
    uint64_t atMs = 0;              /* --at (#205) */
    const char *zLive = NULL, *zKeep = NULL;
    char dir[1024];
    int rc = 0, i, list = 0;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) list = 1;
        else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) zLive = argv[++i];
        else if (strcmp(argv[i], "--keep") == 0 && i + 1 < argc) zKeep = argv[++i];
        else if (strcmp(argv[i], "--to") == 0 && i + 1 < argc && atoll(argv[i + 1]) > 0) stopAt = (uint64_t)atoll(argv[++i]);
        else if (strcmp(argv[i], "--at") == 0 && i + 1 < argc && (atMs = parse_time_ms(argv[i + 1])) > 0) i++;
        else return usage(argv[0]);
    }
    if (list && (zLive || zKeep || stopAt || atMs)) return usage(argv[0]);      /* a listing rebuilds nothing */
    const char *params = getenv("HPPLITE_PARAMS");
    if (!params || !params[0]) {
        fprintf(stderr, "hpplite-verify: HPPLITE_PARAMS must name the chain and the key (l1= or rpc=/factory=/contract=, keyfile=/privkey=)\n");
        return 2;
    }
    if (zKeep) {
        snprintf(dir, sizeof dir, "%s", zKeep);
        (void)!mkdir(dir, 0700);
    } else {
        const char *tmp = getenv("TMPDIR");
        snprintf(dir, sizeof dir, "%s/hpplite-verify-XXXXXX", tmp && tmp[0] ? tmp : "/tmp");
        if (!mkdtemp(dir)) { perror("hpplite-verify: mkdtemp"); return 2; }
    }

    /* store=on when the parameters name a network with a canonical store and no store (#247): on such a network the
    ** bytes - and the databases' names - are at the store, so a verifier without one can do nothing; another store
    ** is named with store= as before */
    const char *zStore = "";
    if (!strstr(params, "store=")) {
        const char *l1 = strncmp(params, "l1=", 3) == 0 ? params + 3 : strstr(params, "&l1=") ? strstr(params, "&l1=") + 4 : NULL;
        if (l1) {
            char alias[64];
            size_t n = strcspn(l1, "&");
            if (n < sizeof alias) {
                memcpy(alias, l1, n); alias[n] = 0;
                const char *url = hpplite_network_store_url(hpplite_da_resolve_alias(alias));
                if (url) { zStore = "&store=on"; fprintf(stderr, "hpplite-verify: store=on (%s, the network's canonical store; give store= to name another)\n", url); }
            }
        }
    }
    /* the verifier's own datadir, first so it wins over one in the parameters. The node reads batch 1 and, when the
    ** store has pruned it, starts from the oldest snapshot kept (#224, #255); the store's batches past the anchor
    ** follow as the soft tier (#166), with --to/--at up to the stop (#390), reported below */
    sqlite3 *db;
    { char to[40] = "";
      if (stopAt) snprintf(to, sizeof to, "&syncto=%llu", (unsigned long long)stopAt);    /* the stop, from the first sync at open */
      else if (atMs) snprintf(to, sizeof to, "&syncat=%llu", (unsigned long long)atMs);
      /* --list (#397): role=list, a verifier that applies nothing */
      char *uri = sqlite3_mprintf("file:%s/rebuilt.db?hpplite=on&role=%s&interval=0&datadir=%s%s&%s%s", dir, list ? "list" : "verify", dir, to, params, zStore);
      db = uri ? hpplite_open(uri) : NULL;
      sqlite3_free(uri); }
    if (!db) {
        fprintf(stderr, "hpplite-verify: cannot open with these parameters (%s)\n", hpplite_config_error() ? hpplite_config_error() : "see above");
        rc = 2;
        goto done;
    }
    HppliteNode *node = hpplite_get_node(db);
    if (!node || !node->l1) {
        const char *why = status_text(db, "last_error");
        if (strstr(why, "no store= names one"))
            fprintf(stderr, "hpplite-verify: could not verify: the wallet's databases are named at a store and no store= names one; give store= (store=on for the network's canonical store)\n");
        else fprintf(stderr, "hpplite-verify: no chain connection (%s)\n", why);
        rc = 2;
        goto close;
    }

    if (list) {
        int lrc = hpplite_node_list(node, list_line, NULL);
        fflush(stdout);
        if (lrc != 0) {
            const char *why = status_text(db, "last_error");
            fprintf(stderr, "hpplite-verify: the listing stopped%s%s\n", why[0] ? ": " : "", why);
            rc = 1;
        }
        goto close;
    }

    /* A long rebuild races the sequencer: batches posted meanwhile are
    ** replayed too, until the chain and the rebuild agree or a sync stops */
    uint64_t onChain = 0, total = 0, rebuilt = 0, target = 0;
    for (i = 0; i < 100; i++) {
        sqlite3_int64 height;
        hpplite_l1_get_da_state(node->l1, &onChain, &total, NULL);
        target = stopAt && stopAt < onChain ? stopAt : onChain;
        height = scalar(db, "SELECT height FROM hpplite_status");   /* next batch number */
        rebuilt = height > 0 ? (uint64_t)height - 1 : 0;
        if (atMs && node->syncAtReached) target = rebuilt;           /* the next batch was written after the time */
        if (rebuilt >= target || node->state == HPPLITE_STATE_ERROR) break;
        if (hpplite_node_sync_from_l1(node, 1) <= 0) {
            /* nothing new applied - but the sync may have settled what the store served past the last anchor, now
            ** anchored: the height is the one after it, not the one read before it (#362, the nightly soak's
            ** "926 batches on chain, 461 replayed" with every root matched) */
            height = scalar(db, "SELECT height FROM hpplite_status");
            rebuilt = height > 0 ? (uint64_t)height - 1 : 0;
            break;
        }
    }
    const char *zState = status_text(db, "state");
    const char *zErr = status_text(db, "last_error");
    sqlite3_int64 fromSnap = scalar(db, "SELECT snapshot_height FROM hpplite_status");
    sqlite3_int64 compared = scalar(db, "SELECT roots_compared FROM hpplite_status");
    sqlite3_int64 mismatches = scalar(db, "SELECT root_mismatches FROM hpplite_status");
    char contract[43] = "", da[43] = "";
    hpplite_l1_get_addresses(node->l1, contract, da);

    /* past the last anchor (#390): the store's batches the node applied up to the stop (soft_height; height stays
    ** the anchored). `reached` is the last batch in the rebuilt file, `rebuilt` the anchored part of it */
    int softTier = node->store && hpplite_l1_has_ranges(node->l1);
    uint64_t reached = rebuilt;
    if ((stopAt || atMs) && softTier && rebuilt >= onChain) {
        sqlite3_int64 soft = scalar(db, "SELECT soft_height FROM hpplite_status");
        if (soft > 0 && (uint64_t)soft - 1 > reached) reached = (uint64_t)soft - 1;
    }
    /* a batch at the store past the anchor that is not the sequencer's or does not apply, between the rebuild and the stop */
    int refused = softTier && rebuilt >= onChain && node->softRefused && ((stopAt && stopAt > reached) || (atMs && !node->syncAtReached));
    if (stopAt && stopAt > onChain && !softTier) {
        fprintf(stderr, "hpplite-verify: --to %llu is past the chain's height %llu\n", (unsigned long long)stopAt, (unsigned long long)onChain);
        rc = 2;
        goto close;
    }
    if (stopAt && stopAt > reached && rebuilt >= onChain && !refused) {
        fprintf(stderr, "hpplite-verify: --to %llu is past the store's last batch %llu (anchored to %llu)\n", (unsigned long long)stopAt, (unsigned long long)reached, (unsigned long long)onChain);
        rc = 2;
        goto close;
    }
    printf("rollup %s: %llu batches on chain, %llu replayed", contract, (unsigned long long)onChain, (unsigned long long)reached);
    if (stopAt) printf(" (--to %llu)", (unsigned long long)stopAt);
    if (fromSnap > 0 && rebuilt >= target && (stopAt || atMs) && !node->triedOldestSnapshot) {
        /* a stop (#391): the state at a point is what is asked for, and the rebuild began at the newest snapshot at or
        ** before it; the batches before it were not read, pruned or not. #232's claim, from that snapshot */
        printf("\nverified from the snapshot at %lld, the newest at or before the stop: the batches before it were not read (without a stop every batch from batch 1 is); "
               "its root is the one the chain anchored there, and %llu batches were replayed after it",
               (long long)fromSnap, (unsigned long long)(reached - (uint64_t)fromSnap));
    } else if (fromSnap > 0 && rebuilt >= target) {
        /* without a stop a verifier starts from a snapshot only when batch 1 could not be read (#239, #255): the prune is the node's finding */
        printf("\nverified from the snapshot at %lld: the batches below it are pruned at the store (keep=); its root is the one the chain anchored there, and %llu batches were replayed after it",
               (long long)fromSnap, (unsigned long long)(rebuilt - (uint64_t)fromSnap));
    }
    if (atMs) {
        char want[40], wrote[40];
        fmt_time(atMs, want, sizeof want);
        if (rebuilt == 0) printf("\nthe database as of %s: no batch written by then", want);
        else if (node->lastAppliedMs == 0) {
            /* a start from a snapshot with no batch replayed after it (#400): the snapshot's own batch is not read,
            ** so there is no writer's time to give */
            printf("\nthe database as of %s: through batch %llu, the snapshot's: no batch after it was written by then", want, (unsigned long long)reached);
        } else {
            fmt_time(node->lastAppliedMs, wrote, sizeof wrote);
            printf("\nthe database as of %s: through batch %llu, written %s%s", want, (unsigned long long)reached, wrote,
                   node->lastAppliedMs > atMs ? " (it completes a transaction in flight at that time)" : "");
        }
    }
    if (rebuilt < target && strstr(zErr, "no store= names one")) {
        /* not a verdict on the data: the bytes are at a store the parameters did not name (#247); what the chain
        ** says is reported below all the same */
        printf("\nhpplite-verify: could not verify: %s; give store= (store=on for the network's canonical store)\n", zErr);
        rc = 2;
    } else if (rebuilt < target) {
        printf("\nhpplite-verify: sync stopped at batch %llu: %s\n", (unsigned long long)rebuilt + 1, zErr[0] ? zErr : zState);
        rc = 1;
    } else if (refused) {
        /* the stop is past it (#390): what was rebuilt is the state before that batch, not the state asked for */
        printf("\nhpplite-verify: batch %llu at the store, past the anchor, is not the sequencer's or does not apply on this history: the rebuild stops before it, "
               "short of the stop\n", (unsigned long long)node->softRefused);
        rc = 1;
    } else {
        printf("\n");
    }
    printf("posted roots compared: %lld, mismatches: %lld", (long long)compared, (long long)mismatches);
    if (mismatches > 0) { printf(" <- the content does not match what was posted"); rc = 1; }
    printf("\n");

    /* the root the contract holds, against the rebuilt content at that height: with ranges (#164) the anchor carries
    ** it, checked in the range block below; without, the checkpoint (#256) */
    HppliteRange lastRange;
    int anchored = onChain > 0 && hpplite_l1_get_range(node->l1, onChain, &lastRange) == 0;
    {
        HppliteL1State *st = hpplite_l1_get_state(node->l1);
        if (stopAt && reached == stopAt) {
            /* the database as of the stop height: its own root, if one was posted there, else the last checked */
            unsigned char at[32];
            char got[65];
            if (hpplite_root_at(node->ctx, stopAt, at) == 0) {
                hex(at, got);
                printf("the database as of batch %llu: state root %s, posted at that height and matched\n", (unsigned long long)stopAt, got);
            } else {
                printf("the database as of batch %llu: no root posted at that height; the last checked was at batch %llu\n",
                       (unsigned long long)stopAt, (unsigned long long)node->ctx->rootCmpHeight);
            }
        }
        if (st && st->lastCheckpointHeight > 0 && stopAt && st->lastCheckpointHeight > stopAt) {
            printf("on-chain checkpoint at batch %llu: past --to %llu, not compared\n", (unsigned long long)st->lastCheckpointHeight, (unsigned long long)stopAt);
        } else if (st && st->lastCheckpointHeight > 0) {
            char want[65], got[65];
            hex(st->lastStateRoot, want);
            unsigned char at[32];
            if (hpplite_root_at(node->ctx, st->lastCheckpointHeight, at) == 0) {
                hex(at, got);
                if (memcmp(st->lastStateRoot, at, 32) == 0) {
                    printf("on-chain checkpoint at batch %llu: root %s matches the rebuilt content\n",
                           (unsigned long long)st->lastCheckpointHeight, want);
                } else {
                    printf("on-chain checkpoint at batch %llu: root %s, rebuilt content %s <- MISMATCH\n",
                           (unsigned long long)st->lastCheckpointHeight, want, got);
                    rc = 1;
                }
            } else {
                printf("on-chain checkpoint at batch %llu (root %s): not reached by the rebuild (last compared: batch %llu)\n",
                       (unsigned long long)st->lastCheckpointHeight, want, (unsigned long long)node->ctx->rootCmpHeight);
                if (rebuilt >= st->lastCheckpointHeight) rc = 1;
            }
        } else if (st && !anchored) {
            printf("no checkpoint on chain yet (checkpoint interval %llu): posted roots only\n", (unsigned long long)st->checkpointInterval);
        }
        hpplite_l1_state_free(st);
    }
    /* the contract's rolling hash over every batch (#33) against the one the rebuild computed */
    {
        uint64_t h = 0;
        uint8_t chain[32];
        if (hpplite_l1_get_chain_hash(node->l1, &h, chain) == 0) {
            const char *mine = status_text(db, "chain_hash");
            char want[65];
            hex(chain, want);
            if (mine[0] == '0' && (mine[1] == 'x' || mine[1] == 'X')) mine += 2;
            if (h == 0) {
                printf("chain hash: no batch on chain yet, nothing to compare\n");
            } else if (rebuilt < h) {
                printf("chain hash: the contract's covers %llu batches, the rebuild %llu: not compared\n", (unsigned long long)h, (unsigned long long)rebuilt);
            } else if (strcasecmp(mine, want) == 0) {
                printf("chain hash over %llu batches: matches the contract's\n", (unsigned long long)h);
            } else {
                printf("chain hash over %llu batches: contract %s, rebuilt %s <- MISMATCH\n", (unsigned long long)h, want, mine[0] ? mine : "(none)");
                rc = 1;
            }
        } else {
            printf("no rolling hash on this contract (DA v1): posted roots and the checkpoint only\n");
        }
    }
    /* where the last batch's bytes are when the chain commits per height (VERSION 8, l1=file://): a reference into a store;
    ** the store's word about them is the bucket statement an anchor carries, reported below (#249) */
    if (onChain > 0) {
        uint8_t h[32], *receipt = NULL;
        char *zRef = NULL;
        size_t nReceipt = 0;
        if (hpplite_l1_get_commitment(node->l1, onChain, h, &zRef, &receipt, &nReceipt) == 0) {
            printf("batch %llu: bytes in a store, reference %s\n", (unsigned long long)onChain, zRef);
            sqlite3_free(zRef);
            sqlite3_free(receipt);
        }
    }
    {
        unsigned char root[32]; char z[65];
        if (hpplite_db_hash(db, root) == SQLITE_OK) { hex(root, z); printf("rebuilt state root: 0x%s\n", z); }
    }
    /* ranges (#164): what the chain anchored against what the rebuild holds */
    if (anchored) {
        HppliteRange r = lastRange;
        {
            char z[65], want[65], got[65];
            unsigned char at[32];
            hex(r.manifestDigest, z);
            printf("anchored to %llu: the last range %llu-%llu, manifest sha-256 %s%s\n", (unsigned long long)onChain,
                   (unsigned long long)r.fromHeight, (unsigned long long)r.toHeight, z,
                   !r.nReceipt ? ", no receipt" : r.receipt[0] == 2 ? ", with the store's bucket statement" : ", with the store's receipt");
            /* the anchor is the checkpoint (#164): the state root it carries against the rebuilt content at its end (#256) */
            hex(r.stateRoot, want);
            if (hpplite_root_at(node->ctx, r.toHeight, at) == 0) {
                hex(at, got);
                if (memcmp(r.stateRoot, at, 32) == 0) printf("anchored root at %llu: %s matches the rebuilt content\n", (unsigned long long)r.toHeight, want);
                else { printf("anchored root at %llu: %s, rebuilt content %s <- MISMATCH\n", (unsigned long long)r.toHeight, want, got); rc = 1; }
            } else printf("anchored root at %llu (%s): not reached by the rebuild\n", (unsigned long long)r.toHeight, want);
            if (r.nReceipt && r.receipt[0] == 2) {
                /* hpplite-store/2 (#215): the store's signed word that it holds the rollup's batches, covered until a time */
                char c[43], d[43], zs[41], when[40];
                unsigned char da[20];
                HppliteStatement stmt;
                const char *zDa;
                int verifies = -1;
                if (hpplite_l1_get_addresses(node->l1, c, d) == 0) {
                    zDa = d[0] ? d : c;
                    if (strlen(zDa) == 42 && hpplite_hex_to_bytes(zDa + 2, da, 20) == 20)
                        verifies = hpplite_store_statement_verify(r.receipt, r.nReceipt, node->config.chainId, da, &stmt);
                }
                if (verifies == 0) {
                    time_t t = (time_t)(stmt.coveredUntil >= HPPLITE_STATEMENT_FOREVER ? 0 : stmt.coveredUntil);
                    hpplite_bytes_to_hex(stmt.signer, 20, zs);
                    strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S UTC", gmtime(&t));
                    printf("bucket statement signed by 0x%s: holds the rollup's names %llu-%llu, covered until %s%s (signature verifies)\n",
                           zs, (unsigned long long)stmt.pruneHeight, (unsigned long long)stmt.height,
                           stmt.coveredUntil >= HPPLITE_STATEMENT_FOREVER ? "no end (a free store)" : when,
                           stmt.coveredUntil < (uint64_t)time(NULL) ? " <- PAST" : "");
                } else {
                    printf("bucket statement: the signature DOES NOT VERIFY\n");
                    rc = 1;
                }
            }
            hpplite_l1_range_clear(&r);
        }
    }

    /* the soft tier (#166): what the store holds under the rollup's names past the anchor, signed by the
    ** sequencer and applied on the rebuilt history by the node itself (soft_height; height stays the anchored) */
    if (stopAt || atMs) {
        /* with a stop (#390), one line in every case: which part of what was rebuilt rests on the chain. Past the
        ** anchor it is the sequencer's signed batches as the store holds them; no root on the chain covers them */
        if (reached == onChain + 1)
            printf("anchored to %llu, rebuilt to %llu: batch %llu is the store's, signed by the rollup's sequencer and applied on the anchored history; "
                   "no anchor on the chain covers it yet\n", (unsigned long long)onChain, (unsigned long long)reached, (unsigned long long)reached);
        else if (reached > onChain)
            printf("anchored to %llu, rebuilt to %llu: batches %llu to %llu are the store's, each signed by the rollup's sequencer and applied on the anchored history; "
                   "no anchor on the chain covers them yet\n", (unsigned long long)onChain, (unsigned long long)reached, (unsigned long long)onChain + 1, (unsigned long long)reached);
        else printf("anchored to %llu, rebuilt to %llu: every batch rebuilt is anchored on the chain\n", (unsigned long long)onChain, (unsigned long long)reached);
    } else if (rc == 0 && node->store && hpplite_l1_has_ranges(node->l1)) {
        sqlite3_int64 soft = scalar(db, "SELECT soft_height FROM hpplite_status");   /* the next batch number past the soft tier */
        uint64_t stored = soft > 0 ? (uint64_t)soft - 1 : onChain;
        if (stored > onChain) printf("stored to %llu, anchored to %llu: %llu batches at the store past the anchor, signed by the sequencer, applied on this history\n",
                                     (unsigned long long)stored, (unsigned long long)onChain, (unsigned long long)(stored - onChain));
        else printf("stored to %llu, anchored to %llu: nothing at the store past the anchor\n", (unsigned long long)onChain, (unsigned long long)onChain);
    }

    if (zLive) {
        sqlite3 *live = NULL;
        if (sqlite3_open_v2(zLive, &live, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            fprintf(stderr, "hpplite-verify: cannot open %s: %s\n", zLive, live ? sqlite3_errmsg(live) : "?");
            rc = 2;
        } else {
            unsigned char a[32], b[32];
            sqlite3_int64 pending = scalar(live, "SELECT count(*) FROM _hpplite_pending");
            if (hpplite_db_hash(live, a) == SQLITE_OK && hpplite_db_hash(db, b) == SQLITE_OK) {
                if (memcmp(a, b, 32) == 0) {
                    printf("live database %s: equals the chain\n", zLive);
                } else {
                    printf("live database %s: DIFFERS from the chain", zLive);
                    if (pending > 0) printf(" (%lld rows pending, not posted yet: flush, then verify again)", (long long)pending);
                    printf("\n");
                    int n = report_table_differences(live, db);
                    if (n == 0) printf("  (no table differs row for row: the schema text differs)\n");
                    rc = rc ? rc : 1;
                }
            } else {
                fprintf(stderr, "hpplite-verify: cannot hash %s\n", zLive);
                rc = 2;
            }
        }
        sqlite3_close(live);
    }
    printf("%s\n", rc == 0 ? "verified" : rc == 1 ? "NOT verified" : "could not verify");

close:
    sqlite3_close(db);
done:
    if (!zKeep) nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    else printf("rebuilt database kept in %s\n", dir);
    return rc;
}
