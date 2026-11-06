/*
** HPPLite DA URI Scheme
**
** URI format: <scheme>://<chainId>/<contract>/<height>
** Examples:
**   hppda://181228/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/42
**   hppda://hpp-sepolia/0x2Cd27d02C3b36c8926B849Bc53745Fab661572Dc/1-10
**   ipfs://QmHash
**   file:///path/to/batch.json
*/

#ifndef HPPLITE_DA_URI_H
#define HPPLITE_DA_URI_H

#include "l1_interface.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
** DA URI schemes
*/
typedef enum {
    HPPLITE_DA_UNKNOWN = 0,
    HPPLITE_DA_HPPDA,       /* On-chain L1 DA */
    HPPLITE_DA_IPFS,        /* IPFS */
    HPPLITE_DA_FILE,        /* Local file */
    HPPLITE_DA_HTTP         /* HTTP/HTTPS */
} HppliteDAScheme;

/*
** Parsed DA URI
*/
typedef struct HppliteDAUri {
    HppliteDAScheme scheme;

    /* For hppda:// */
    uint64_t chainId;
    unsigned char contract[20];
    uint64_t heightFrom;
    uint64_t heightTo;          /* Same as heightFrom for single batch */

    /* For ipfs://, file://, http:// */
    char *path;

    /* Original URI string */
    char *raw;
} HppliteDAUri;

/*
** Network alias registry
*/
typedef struct HppliteNetworkAlias {
    const char *alias;
    uint64_t chainId;
    const char *rpcUrl;
    const char *factory;    /* Factory contract address (hex string) */
    size_t maxTxBytes;      /* Largest batch to post in one transaction (0 = unknown) */
    uint64_t maxTxGas;      /* Gas cap per transaction (0 = unknown) */
    int batchIntervalMs;    /* The chain's own cadence: the batch interval unless interval= says otherwise (0 = the default) */
    const char *storeUrl;   /* the canonical store, what store=on takes (#192); NULL: none on this network */
    const char *relayUrl;   /* the canonical relay, what relay=on takes */
} HppliteNetworkAlias;

/* The limits of a known chain, by chain id (0 = unknown) */
size_t hpplite_network_max_tx_bytes(uint64_t chainId);
uint64_t hpplite_network_max_tx_gas(uint64_t chainId);
int hpplite_network_batch_interval_ms(uint64_t chainId);   /* 0 when the chain sets none */

/*
** Built-in network aliases
*/
extern const HppliteNetworkAlias HPPLITE_NETWORKS[];
extern const int HPPLITE_NETWORKS_COUNT;

/*
** Parse a DA URI string.
** Returns allocated HppliteDAUri or NULL on error.
** Caller must free with hpplite_da_uri_free().
*/
HppliteDAUri *hpplite_da_uri_parse(const char *uri);

/*
** Free a parsed DA URI.
*/
void hpplite_da_uri_free(HppliteDAUri *uri);

/*
** Build a DA URI string from components.
** Caller must free returned string with free().
*/
char *hpplite_da_uri_build(
    HppliteDAScheme scheme,
    uint64_t chainId,
    const unsigned char *contract,
    uint64_t heightFrom,
    uint64_t heightTo
);

/*
** Build a DA URI from system config and height.
** Caller must free returned string with free().
*/
char *hpplite_da_uri_from_config(
    const HppliteSystemConfig *config,
    uint64_t heightFrom,
    uint64_t heightTo
);

/*
** Resolve network alias to chain ID.
** Returns chain ID or 0 if not found.
*/
uint64_t hpplite_da_resolve_alias(const char *alias);

/*
** Get RPC URL for chain ID.
** Returns static string or NULL if not found.
*/
const char *hpplite_da_get_rpc_url(uint64_t chainId);
/* The network's canonical store and relay (#192): what store=on and relay=on take; NULL when it has none */
const char *hpplite_network_store_url(uint64_t chainId);
const char *hpplite_network_relay_url(uint64_t chainId);

/*
** Get factory address for chain ID.
** Returns static string or NULL if not found.
*/
const char *hpplite_da_get_factory(uint64_t chainId);

/*
** Look up network by alias.
** Returns pointer to static entry or NULL if not found.
*/
const HppliteNetworkAlias *hpplite_da_lookup_network(const char *alias);

/*
** Get scheme string from enum.
*/
const char *hpplite_da_scheme_str(HppliteDAScheme scheme);

/*
** Parse scheme string to enum.
*/
HppliteDAScheme hpplite_da_scheme_parse(const char *str);

#ifdef __cplusplus
}
#endif

#endif /* HPPLITE_DA_URI_H */
