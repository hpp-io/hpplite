/*
** hpplite-sequencer: hold a database open as its sequencer.
**
**   HPPLITE_PARAMS="rpc=...&factory=...&chainid=...&privkey=0x...&interval=1s" \
**     hpplite-sequencer /path/to/app.db
**
** For apps whose connections are short-lived - PHP (php-fpm, one connection
** per request), CGI, cron jobs: every HPPLite connection to a file captures
** into its shared log, and one of them, the holder of <file>-hpplite, posts.
** A connection that closes while it sequences posts what is pending first,
** so without this process some request pays for the chain. With it, every
** application connection is a follower: it captures and closes at once, and
** this process posts, on the interval, for all of them. It takes the same
** HPPLITE_PARAMS as the application. Stop it with SIGTERM or SIGINT: it posts
** what is left, and an application connection takes over if any is open.
**
** The first line on stdout, "hpplite-sequencer: <file> synced height N", comes
** when the open has returned (#264): the chain and the store applied to the
** file, the lease taken. A supervisor restoring an empty volume waits for it
** before the application opens the file - pages cannot be applied while
** another connection holds the file in WAL mode. Everything else goes to stderr.
**
** A fleet in one process (somaro, decision 0016):
**
**   hpplite-sequencer --dir /etc/hpplite/fleet
**
** holds every database named by a file <name>.conf in the directory, each its
** own connection, key and rollup. A file is key=value lines - db=<path>,
** params=<HPPLITE_PARAMS, with contract=: a fleet opens by address, never by
** name>, privkey=0x... - and must be readable by its owner alone (mode 0600):
** the keys are never on the command line nor in one shared environment.
** SIGHUP reads the directory again: a new file is opened, a removed one is
** closed (what is pending posted), a changed one is closed and opened again,
** one that failed to open is tried again; the others are not disturbed. One
** line per database on stdout, prefixed with its name: "<name>: synced height
** N", "<name>: closed", "<name>: error: ...", and the sequencer role as it
** changes. One database failing does not stop the rest. SIGTERM or SIGINT
** closes them all, each posting what is pending, within
** HPPLITE_STOP_SECONDS (default 30); a database not closed by then is named
** and keeps its pending pages in its file for the next start (exit 1).
*/

#include "hpplite.h"
#include "config.h"
#include <dirent.h>
#include <stdarg.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stop, reread;
static void on_signal(int sig) { (void)sig; stop = 1; }
static void on_hup(int sig) { (void)sig; reread = 1; }

static int status_int(sqlite3 *db, const char *col) {
    char sql[96];
    sqlite3_stmt *st;
    int v = -1;
    snprintf(sql, sizeof(sql), "SELECT %s FROM hpplite_status", col);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return -1;
    if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return v;
}

static char *status_text(sqlite3 *db, const char *col, char *out, size_t n) {
    char sql[96];
    sqlite3_stmt *st;
    out[0] = 0;
    snprintf(sql, sizeof(sql), "SELECT %s FROM hpplite_status", col);
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) return out;
    if (sqlite3_step(st) == SQLITE_ROW && sqlite3_column_text(st, 0)) snprintf(out, n, "%s", (const char *)sqlite3_column_text(st, 0));
    sqlite3_finalize(st);
    return out;
}

/* ---- the fleet: one connection per <name>.conf, each in its own thread ---- */

typedef struct Db Db;
struct Db {
    char name[128];
    uint64_t hash;                  /* of the file's bytes: a changed file is closed and opened again */
    char *uri;                      /* with the key: freed, wiped, once opened */
    pthread_t th;
    volatile int started, quit, done, failed;
    unsigned gen;                   /* the last scan that found its file */
    Db *after;                      /* the entry this one replaces: it opens once that one is closed */
    Db *next;
};
static Db *fleet;
static pthread_mutex_t outMutex = PTHREAD_MUTEX_INITIALIZER;

static void say(const char *name, const char *fmt, ...) {
    va_list ap;
    pthread_mutex_lock(&outMutex);
    printf("%s: ", name);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);                 /* a supervisor waits on the lines */
    pthread_mutex_unlock(&outMutex);
}

static void wipe_free(char *z) {
    if (!z) return;
    volatile char *p = z;
    while (*p) *p++ = 0;
    sqlite3_free(z);
}

static void *db_thread(void *arg) {
    Db *d = (Db *)arg;
    sqlite3 *db = hpplite_open(d->uri);
    char err[512] = "", seen[512] = "", state[32];
    int role = -1;
    wipe_free(d->uri);
    d->uri = NULL;
    if (!db) {
        const char *why = hpplite_config_error();
        say(d->name, "error: cannot open with HPPLite%s%s", why ? ": " : "", why ? why : " (its params?)");
        d->failed = 1;
        d->done = 1;
        return NULL;
    }
    say(d->name, "synced height %d", status_int(db, "height"));
    while (!d->quit) {
        int seq = status_int(db, "sequencer");
        if (seq != role) {
            role = seq;
            say(d->name, "%s", seq == 1 ? "sequencing" : "waiting: another process sequences it");
        }
        status_text(db, "state", state, sizeof state);
        status_text(db, "last_error", err, sizeof err);
        if (strcmp(state, "error") == 0 && strcmp(err, seen) != 0) say(d->name, "error: %s", err);
        snprintf(seen, sizeof seen, "%s", strcmp(state, "error") == 0 ? err : "");
        for (int i = 0; i < 10 && !d->quit; i++) usleep(100000);
    }
    sqlite3_close(db);              /* what is pending is posted */
    say(d->name, "closed");
    d->done = 1;
    return NULL;
}

static uint64_t fnv(const char *z, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) { h ^= (unsigned char)z[i]; h *= 1099511628211ULL; }
    return h;
}

/* <dir>/<file>: its URI and the hash of its bytes, or NULL with the reason said */
static char *read_conf(const char *dir, const char *file, const char *name, uint64_t *hash) {
    char path[1024], buf[8192], *db = NULL, *params = NULL, *key = NULL, *uri = NULL;
    struct stat st;
    size_t n;
    FILE *f;
    snprintf(path, sizeof path, "%s/%s", dir, file);
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return NULL;
    if (st.st_mode & 077) { say(name, "error: %s is readable by others than its owner (chmod 600): not opened", path); return NULL; }
    if (!(f = fopen(path, "r"))) { say(name, "error: cannot read %s", path); return NULL; }
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    *hash = fnv(buf, n);
    for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
        size_t m = strlen(line);
        while (m && (line[m - 1] == '\r' || line[m - 1] == ' ')) line[--m] = 0;
        if (!strncmp(line, "db=", 3)) db = line + 3;
        else if (!strncmp(line, "params=", 7)) params = line + 7;
        else if (!strncmp(line, "privkey=", 8)) key = line + 8;
    }
    if (!db || !db[0]) say(name, "error: %s names no db=", path);
    else if (!params || !(strncmp(params, "contract=", 9) == 0 || strstr(params, "&contract=")))
        say(name, "error: %s: params= must name the rollup by contract= (a fleet opens by address)", path);
    else uri = sqlite3_mprintf("file:%s?hpplite=on&%s%s%s", db, params, key && key[0] ? "&privkey=" : "", key ? key : "");
    memset(buf, 0, sizeof buf);
    return uri;
}

static void start_db(Db *d) {
    d->started = 1;
    if (pthread_create(&d->th, NULL, db_thread, d) != 0) { say(d->name, "error: no thread"); d->failed = 1; d->done = 1; d->started = 0; }
}

/* the directory read again: added, removed, changed and failed entries */
static void fleet_scan(const char *dir) {
    static unsigned gen;
    DIR *dh = opendir(dir);
    struct dirent *e;
    Db *d;
    if (!dh) { fprintf(stderr, "hpplite-sequencer: cannot read %s\n", dir); return; }   /* nothing closed for a directory that did not read */
    gen++;
    while ((e = readdir(dh))) {
        size_t m = strlen(e->d_name);
        char name[128];
        uint64_t hash = 0;
        if (e->d_name[0] == '.' || m <= 5 || strcmp(e->d_name + m - 5, ".conf") != 0 || m - 5 >= sizeof name) continue;
        snprintf(name, sizeof name, "%.*s", (int)(m - 5), e->d_name);
        Db *cur = NULL;
        for (d = fleet; d; d = d->next) if (!d->quit && strcmp(d->name, name) == 0) cur = d;
        if (cur) cur->gen = gen;                    /* its file is there: kept, whatever it reads */
        char *uri = read_conf(dir, e->d_name, name, &hash);
        if (!uri) continue;
        if (cur && cur->hash == hash && !cur->failed) { wipe_free(uri); continue; }   /* unchanged, open */
        Db *nd = calloc(1, sizeof *nd);
        if (!nd) { wipe_free(uri); continue; }
        snprintf(nd->name, sizeof nd->name, "%s", name);
        nd->hash = hash;
        nd->gen = gen;
        nd->uri = uri;
        if (cur) { cur->quit = 1; if (!cur->done) nd->after = cur; }   /* changed or failed: the old one goes first */
        nd->next = fleet;
        fleet = nd;
        if (!nd->after) start_db(nd);
    }
    closedir(dh);
    for (d = fleet; d; d = d->next) if (!d->quit && d->gen != gen) d->quit = 1;   /* a file gone: its database closes */
}

/* closed entries joined and freed; replacements started once what they replace is closed */
static void fleet_reap(void) {
    for (Db *d = fleet; d; d = d->next) if (d->after && d->after->done) { d->after = NULL; start_db(d); }
    Db **pp = &fleet;
    while (*pp) {
        Db *d = *pp;
        int referenced = 0;
        for (Db *o = fleet; o; o = o->next) if (o->after == d) referenced = 1;
        if (d->done && (d->quit || d->failed) && !referenced) {
            if (d->started) pthread_join(d->th, NULL);
            d->started = 0;
            if (d->quit) { *pp = d->next; wipe_free(d->uri); free(d); continue; }
        }
        pp = &d->next;
    }
}

static int fleet_main(const char *dir) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_hup;
    sigaction(SIGHUP, &sa, NULL);
    fleet_scan(dir);
    while (!stop) {
        if (reread) { reread = 0; fleet_scan(dir); }
        fleet_reap();
        usleep(200000);
    }
    const char *z = getenv("HPPLITE_STOP_SECONDS");
    int secs = z && atoi(z) > 0 ? atoi(z) : 30, open = 0;
    time_t until = time(NULL) + secs;
    fprintf(stderr, "hpplite-sequencer: stopping; every database posts what is pending (at most %d s)\n", secs);
    for (Db *d = fleet; d; d = d->next) { d->quit = 1; if (d->after) d->after = NULL; }   /* a replacement not started stays unstarted */
    for (;;) {
        open = 0;
        for (Db *d = fleet; d; d = d->next) if (d->started && !d->done) open++;
        if (!open || time(NULL) >= until) break;
        usleep(100000);
    }
    for (Db *d = fleet; d; d = d->next) if (d->started && !d->done) say(d->name, "error: not closed within %d s: its pending pages stay in its file for the next start", secs);
    return open ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "--dir") == 0) return fleet_main(argv[2]);
    if (argc != 2) {
        fprintf(stderr, "usage: HPPLITE_PARAMS=... %s <database file>\n       %s --dir <directory of <name>.conf files>\n", argv[0], argv[0]);
        return 2;
    }
    const char *params = getenv("HPPLITE_PARAMS");
    char *uri = sqlite3_mprintf("file:%s?hpplite=on%s%s", argv[1],
                                params && params[0] ? "&" : "", params ? params : "");
    sqlite3 *db = uri ? hpplite_open(uri) : NULL;
    sqlite3_free(uri);
    if (!db) {
        fprintf(stderr, "hpplite-sequencer: cannot open %s with HPPLite (HPPLITE_PARAMS?)\n", argv[1]);
        return 1;
    }
    printf("hpplite-sequencer: %s synced height %d\n", argv[1], status_int(db, "height"));
    fflush(stdout);                                /* the line is the signal: out before anything else happens */
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    int role = -1;
    while (!stop) {
        int seq = status_int(db, "sequencer");
        if (seq != role) {
            role = seq;
            fprintf(stderr, "hpplite-sequencer: %s %s\n", argv[1],
                    seq == 1 ? "sequencing" : "waiting: another process sequences it");
        }
        sleep(1);
    }
    fprintf(stderr, "hpplite-sequencer: stopping; posting what is pending\n");
    sqlite3_close(db);
    return 0;
}
