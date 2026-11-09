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
*/

#include "hpplite.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: HPPLITE_PARAMS=... %s <database file>\n", argv[0]);
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
