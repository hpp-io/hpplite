/*
** The spend ledger (#173): see spend.h.
*/
#include "spend.h"
#include "cJSON.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define SPEND_KEEP (24 * 3600)          /* a day of entries: the longest window and the rate's basis */
#define SPEND_MAX  20000                /* entries kept at most (a put every 5 s is 17,280 a day) */

typedef struct { int64_t t; uint64_t amount; } Entry;
typedef struct { int64_t t; double wei; } Sample;

struct HppliteSpend {
    pthread_mutex_t mu;
    char *path;
    uint64_t cap, window;
    Entry *a; int n;
    Sample *s; int ns;
    int64_t savedAt;
};

static int64_t now_s(void) { return (int64_t)time(NULL); }

static void trim(HppliteSpend *sp, int64_t now) {
    int i = 0, k;
    while (i < sp->n && (sp->a[i].t < now - SPEND_KEEP || sp->n - i > SPEND_MAX)) i++;
    if (i) { memmove(sp->a, sp->a + i, (size_t)(sp->n - i) * sizeof *sp->a); sp->n -= i; }
    for (k = 0; k < sp->ns && sp->s[k].t < now - SPEND_KEEP; k++) {}
    if (k && sp->ns - k >= 1) { memmove(sp->s, sp->s + k, (size_t)(sp->ns - k) * sizeof *sp->s); sp->ns -= k; }
}

static void save(HppliteSpend *sp) {
    FILE *f;
    char tmp[1100];
    int i;
    if (!sp->path) return;
    snprintf(tmp, sizeof tmp, "%s.tmp", sp->path);
    if (!(f = fopen(tmp, "w"))) return;
    fprintf(f, "{\"charges\":[");
    for (i = 0; i < sp->n; i++) fprintf(f, "%s[%lld,%llu]", i ? "," : "", (long long)sp->a[i].t, (unsigned long long)sp->a[i].amount);
    fprintf(f, "],\"eth\":[");
    for (i = 0; i < sp->ns; i++) fprintf(f, "%s[%lld,%.0f]", i ? "," : "", (long long)sp->s[i].t, sp->s[i].wei);
    fprintf(f, "]}\n");
    if (fclose(f) == 0) rename(tmp, sp->path);
    sp->savedAt = now_s();
}

static void load(HppliteSpend *sp) {
    FILE *f = sp->path ? fopen(sp->path, "r") : NULL;
    char *buf;
    long len;
    cJSON *j, *it;
    if (!f) return;
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 4 * 1024 * 1024 || !(buf = malloc((size_t)len + 1))) { fclose(f); return; }
    len = (long)fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[len] = 0;
    j = cJSON_Parse(buf);
    free(buf);
    cJSON *charges = cJSON_GetObjectItem(j, "charges"), *eth = cJSON_GetObjectItem(j, "eth");
    cJSON_ArrayForEach(it, charges) {
        cJSON *t = cJSON_GetArrayItem(it, 0), *v = cJSON_GetArrayItem(it, 1);
        Entry *na;
        if (!cJSON_IsNumber(t) || !cJSON_IsNumber(v) || !(na = realloc(sp->a, (size_t)(sp->n + 1) * sizeof *sp->a))) continue;
        sp->a = na;
        sp->a[sp->n].t = (int64_t)t->valuedouble;
        sp->a[sp->n].amount = (uint64_t)v->valuedouble;
        sp->n++;
    }
    cJSON_ArrayForEach(it, eth) {
        cJSON *t = cJSON_GetArrayItem(it, 0), *v = cJSON_GetArrayItem(it, 1);
        Sample *ns;
        if (!cJSON_IsNumber(t) || !cJSON_IsNumber(v) || !(ns = realloc(sp->s, (size_t)(sp->ns + 1) * sizeof *sp->s))) continue;
        sp->s = ns;
        sp->s[sp->ns].t = (int64_t)t->valuedouble;
        sp->s[sp->ns].wei = v->valuedouble;
        sp->ns++;
    }
    cJSON_Delete(j);
    trim(sp, now_s());
}

HppliteSpend *hpplite_spend_open(const char *zPath, uint64_t cap, uint64_t window) {
    HppliteSpend *sp = calloc(1, sizeof *sp);
    if (!sp) return NULL;
    pthread_mutex_init(&sp->mu, NULL);
    sp->path = zPath ? strdup(zPath) : NULL;
    sp->cap = cap;
    sp->window = window ? window : 86400;
    load(sp);
    return sp;
}

void hpplite_spend_close(HppliteSpend *sp) {
    if (!sp) return;
    pthread_mutex_lock(&sp->mu);
    save(sp);
    pthread_mutex_unlock(&sp->mu);
    pthread_mutex_destroy(&sp->mu);
    free(sp->a); free(sp->s); free(sp->path);
    free(sp);
}

static uint64_t sum_since(HppliteSpend *sp, int64_t from) {
    uint64_t total = 0;
    for (int i = sp->n - 1; i >= 0 && sp->a[i].t >= from; i--) total += sp->a[i].amount;
    return total;
}

int hpplite_spend_allow(HppliteSpend *sp, uint64_t amount, char *zWhy, size_t nWhy) {
    uint64_t used;
    int ok;
    if (!sp || !sp->cap) return 0;
    pthread_mutex_lock(&sp->mu);
    used = sum_since(sp, now_s() - (int64_t)sp->window);
    ok = used + amount <= sp->cap;
    pthread_mutex_unlock(&sp->mu);
    if (!ok && zWhy) snprintf(zWhy, nWhy, "this call (%llu) would pass spendcap=%llu/%s (%llu spent in it)", (unsigned long long)amount,
                              (unsigned long long)sp->cap, sp->window == 3600 ? "hour" : "day", (unsigned long long)used);
    return ok ? 0 : -1;
}

void hpplite_spend_add(HppliteSpend *sp, uint64_t amount) {
    Entry *na;
    int64_t now = now_s();
    if (!sp || !amount) return;
    pthread_mutex_lock(&sp->mu);
    trim(sp, now);
    if ((na = realloc(sp->a, (size_t)(sp->n + 1) * sizeof *sp->a))) {
        sp->a = na;
        sp->a[sp->n].t = now;
        sp->a[sp->n].amount = amount;
        sp->n++;
    }
    save(sp);
    pthread_mutex_unlock(&sp->mu);
}

uint64_t hpplite_spend_in_window(HppliteSpend *sp) {
    uint64_t v;
    if (!sp) return 0;
    pthread_mutex_lock(&sp->mu);
    v = sum_since(sp, now_s() - (int64_t)sp->window);
    pthread_mutex_unlock(&sp->mu);
    return v;
}

uint64_t hpplite_spend_since(HppliteSpend *sp, uint64_t seconds, uint64_t *pSpan) {
    uint64_t v;
    int64_t now = now_s(), from = now - (int64_t)seconds;
    if (pSpan) *pSpan = 0;
    if (!sp) return 0;
    pthread_mutex_lock(&sp->mu);
    v = sum_since(sp, from);
    if (pSpan && sp->n) {
        int64_t first = sp->a[0].t > from ? sp->a[0].t : from;
        *pSpan = (uint64_t)(now - first);
    }
    pthread_mutex_unlock(&sp->mu);
    return v;
}

uint64_t hpplite_spend_cap(HppliteSpend *sp, uint64_t *pWindow) {
    if (pWindow) *pWindow = sp ? sp->window : 0;
    return sp ? sp->cap : 0;
}

void hpplite_spend_eth_sample(HppliteSpend *sp, double wei) {
    Sample *ns;
    int64_t now = now_s();
    if (!sp) return;
    pthread_mutex_lock(&sp->mu);
    trim(sp, now);
    if (sp->ns && now - sp->s[sp->ns - 1].t < 10) {
        sp->s[sp->ns - 1].wei = wei;                  /* at most one sample per 10 s: the latest */
    } else if ((ns = realloc(sp->s, (size_t)(sp->ns + 1) * sizeof *sp->s))) {
        sp->s = ns;
        sp->s[sp->ns].t = now;
        sp->s[sp->ns].wei = wei;
        sp->ns++;
    }
    if (now - sp->savedAt >= 60) save(sp);
    pthread_mutex_unlock(&sp->mu);
}

double hpplite_spend_eth_spent(HppliteSpend *sp, uint64_t *pSpan) {
    double spent = 0;
    if (pSpan) *pSpan = 0;
    if (!sp) return 0;
    pthread_mutex_lock(&sp->mu);
    for (int i = 1; i < sp->ns; i++)
        if (sp->s[i].wei < sp->s[i - 1].wei) spent += sp->s[i - 1].wei - sp->s[i].wei;
    if (pSpan && sp->ns >= 2) *pSpan = (uint64_t)(sp->s[sp->ns - 1].t - sp->s[0].t);
    pthread_mutex_unlock(&sp->mu);
    return spent;
}

int hpplite_spend_parse_cap(const char *z, uint64_t *pCap, uint64_t *pWindow) {
    char *end;
    unsigned long long v;
    if (!z || !*z) return -1;
    v = strtoull(z, &end, 10);
    if (end == z || v == 0) return -1;
    *pCap = (uint64_t)v;
    *pWindow = 86400;
    if (*end == 0) return 0;
    if (*end != '/') return -1;
    end++;
    if (strcasecmp(end, "hour") == 0 || strcasecmp(end, "h") == 0) *pWindow = 3600;
    else if (strcasecmp(end, "day") == 0 || strcasecmp(end, "d") == 0) *pWindow = 86400;
    else return -1;
    return 0;
}
