/*
** The certificates HPPLite's HTTPS clients trust (#269, #377): the chain RPC's (eth/eth_client.c) and the
** store's and the relay's (store.c). HPPLITE_CA_BUNDLE names the file; without it libcurl's own default
** applies, and where that default is a file that is not there - the static build's, compiled on Alpine and
** run on another distribution - the distributions' usual bundles are tried in turn.
*/
#ifndef HPPLITE_TLS_H
#define HPPLITE_TLS_H

#include <curl/curl.h>
#include <stddef.h>

/* Set the bundle on a handle, before each request (curl_easy_reset() forgets it) */
void hpplite_tls_trust(CURL *c);

/* 1 where rc is a certificate that could not be checked: z gets the line that says so - the host, the
** bundle it was checked against or that none was found, and the remedy - and the thread keeps it for
** hpplite_tls_why(). 0 for any other failure, z untouched */
int hpplite_tls_failed(CURLcode rc, const char *zUrl, char *z, size_t n);

/* This thread's last such line, NULL where none since hpplite_tls_why_clear(): for a caller whose client
** is out of reach (the factory's lookups make their own) */
const char *hpplite_tls_why(void);
void hpplite_tls_why_clear(void);

#endif
