/*
** What the node pays, over time (#173): a ledger of the charges the store
** and the relay made (atomic units of their asset, USDC.e), kept a day in
** the data directory so a restart does not reset it; a cap over a rolling
** window (spendcap=); samples of the wallet's ETH balance, whose fall is
** what its own transactions cost. One ledger per node, shared by the store
** and the relay handles; safe from the flush thread and the status reader.
*/
#ifndef HPPLITE_SPEND_H
#define HPPLITE_SPEND_H

#include <stddef.h>
#include <stdint.h>

typedef struct HppliteSpend HppliteSpend;

/* zPath: the ledger's file (NULL: memory only); cap 0: no cap; window in seconds (3600 or 86400) */
HppliteSpend *hpplite_spend_open(const char *zPath, uint64_t cap, uint64_t window);
void hpplite_spend_close(HppliteSpend *sp);

/* 0 when paying amount now stays within the cap; -1 with the reason in zWhy (it names spendcap=) */
int hpplite_spend_allow(HppliteSpend *sp, uint64_t amount, char *zWhy, size_t nWhy);
/* A charge made now */
void hpplite_spend_add(HppliteSpend *sp, uint64_t amount);
/* Charged in the cap's window, and in the last `seconds` (the rate's basis); the age of the oldest entry kept */
uint64_t hpplite_spend_in_window(HppliteSpend *sp);
uint64_t hpplite_spend_since(HppliteSpend *sp, uint64_t seconds, uint64_t *pSpan);
uint64_t hpplite_spend_cap(HppliteSpend *sp, uint64_t *pWindow);

/* The wallet's ETH balance now (wei); what it fell by over the last day (rises, a top-up, are not spending) and over what span */
void hpplite_spend_eth_sample(HppliteSpend *sp, double wei);
double hpplite_spend_eth_spent(HppliteSpend *sp, uint64_t *pSpan);

/* "spendcap=5000/hour" etc.: the cap and its window; -1 when it does not parse */
int hpplite_spend_parse_cap(const char *z, uint64_t *pCap, uint64_t *pWindow);

#endif
