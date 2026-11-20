/*
** The certificates HPPLite's HTTPS clients trust (#269, #377). See tls.h.
*/
#include "tls.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static _Thread_local char tlWhy[300];

/* libcurl's own default bundle where it is a file: NULL where its TLS library keeps the certificates
** another way (a directory, the system's store) */
static const char *curlDefault(void) {
#if LIBCURL_VERSION_NUM >= 0x074600
    curl_version_info_data *v = curl_version_info(CURLVERSION_NOW);
    if (v && v->age >= CURLVERSION_TENTH) return v->cainfo;
#endif
    return NULL;
}

/* The bundle to set on a handle, NULL to leave libcurl its own. *pSearched: libcurl's own file is not
** there, and the usual ones were looked for */
static const char *caBundle(int *pSearched) {
    static const char *const usual[] = {
        "/etc/ssl/certs/ca-certificates.crt",       /* Debian, Ubuntu, Alpine */
        "/etc/pki/tls/certs/ca-bundle.crt",         /* Fedora, RHEL */
        "/etc/ssl/cert.pem",                        /* Alpine, macOS */
        "/etc/ssl/ca-bundle.pem",                   /* openSUSE */
    };
    const char *z = getenv("HPPLITE_CA_BUNDLE"), *zOwn;
    *pSearched = 0;
    if (z) return z;
    zOwn = curlDefault();
    if (!zOwn || access(zOwn, R_OK) == 0) return NULL;
    *pSearched = 1;
    for (size_t i = 0; i < sizeof usual / sizeof usual[0]; i++) if (access(usual[i], R_OK) == 0) return usual[i];
    return NULL;
}

void hpplite_tls_trust(CURL *c) {
    int searched;
    const char *z = caBundle(&searched);
    if (z) curl_easy_setopt(c, CURLOPT_CAINFO, z);
}

/* The host of a URL, alone: an RPC's URL can carry its key in the path, or before an @ */
static void urlHost(const char *zUrl, char *host, size_t n) {
    const char *p = zUrl ? strstr(zUrl, "://") : NULL, *at;
    size_t len, e;
    p = p ? p + 3 : (zUrl ? zUrl : "");
    len = strcspn(p, "/?#");
    for (at = p + len; at > p && at[-1] != '@'; at--) {}
    len -= (size_t)(at - p);
    p = at;
    if (p[0] == '[') { e = strcspn(p, "]"); if (e < len) len = e + 1; }         /* an IPv6 literal keeps its colons */
    else { e = strcspn(p, ":"); if (e < len) len = e; }
    snprintf(host, n, "%.*s", (int)len, p);
}

int hpplite_tls_failed(CURLcode rc, const char *zUrl, char *z, size_t n) {
    char host[128];
    const char *zBundle;
    int searched;
    if (rc != CURLE_PEER_FAILED_VERIFICATION && rc != CURLE_SSL_CACERT_BADFILE) return 0;
    urlHost(zUrl, host, sizeof host);
    zBundle = caBundle(&searched);
    if (getenv("HPPLITE_CA_BUNDLE")) snprintf(z, n, "the certificate of %s could not be checked against %s (HPPLITE_CA_BUNDLE)", host, zBundle);
    else if (searched && !zBundle) snprintf(z, n, "the certificate of %s could not be checked: no CA bundle found; set HPPLITE_CA_BUNDLE", host);
    else {
        if (!zBundle) zBundle = curlDefault();
        snprintf(z, n, "the certificate of %s could not be checked against %s; HPPLITE_CA_BUNDLE names another bundle", host, zBundle ? zBundle : "libcurl's own certificates");
    }
    snprintf(tlWhy, sizeof tlWhy, "%s", z);
    return 1;
}

const char *hpplite_tls_why(void) { return tlWhy[0] ? tlWhy : NULL; }
void hpplite_tls_why_clear(void) { tlWhy[0] = 0; }
