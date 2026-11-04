/* x402_upto_channel.h - Cumulative x402 'upto' authorization accounting (#135) */
#ifndef X402_UPTO_CHANNEL_H
#define X402_UPTO_CHANNEL_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    char payer_address[44];
    uint64_t nonce;
    uint64_t valid_before_unix;
    uint64_t max_authorized_units;
    uint64_t cumulative_spent_units;
} x402_upto_session_t;

static inline bool x402_upto_can_draw(
    const x402_upto_session_t *session,
    uint64_t put_cost_units,
    uint64_t now_unix
) {
    if (!session) return false;
    if (now_unix >= session->valid_before_unix) return false;
    if (put_cost_units > (session->max_authorized_units - session->cumulative_spent_units)) {
        return false;
    }
    return true;
}

static inline bool x402_upto_record_put(
    x402_upto_session_t *session,
    uint64_t put_cost_units,
    uint64_t now_unix
) {
    if (!x402_upto_can_draw(session, put_cost_units, now_unix)) {
        return false;
    }
    session->cumulative_spent_units += put_cost_units;
    return true;
}

#endif /* X402_UPTO_CHANNEL_H */
