# Reference relay (#170)

`relay.py` submits an HPPLite node's signed contract calls and pays their
gas, charged over x402 in USDC.e. With it, a node whose wallet holds only
USDC.e can create its rollup, take and renew its lease, and anchor. With the
pin service beside it ([../store](../store/README.md)), it needs no ETH
at all. Standard library Python. It shares `../x402seller` with the store:
`eth.py` (keccak, secp256k1, EIP-712), the x402 seller code and the channels.

```sh
relay.py --port 8430 --rpc https://sepolia.hpp.io --key-file relay.key \
    --factory 0xbce61e56f84620bc4365e95126f82b51faed7b4e \
    --facilitator https://facilitator-sepolia.hpp.io
```

## What it does

`POST /commit` takes `{"to": address, "data": calldata}`:

1. **Is it a call the relay takes?** Only the EIP-712 signed forms, and
   only on the factory it serves (VERSION 10 or later) or on a rollup or DA
   contract that factory created:
   - the factory's `createRollupFor`;
   - a rollup's `claimSequencerFor`, `renewLeaseFor`, `releaseLeaseFor`,
     `submitCheckpointFor` and `setCheckpointIntervalFor`;
   - a DA contract's `commitRangeFor`, `submitBatchFor` and
     `commitBatchFor`.

   Anything else gets `400` before any simulation. The relay's ETH pays for
   HPPLite, not for whatever a caller sends.
2. **Would it succeed?** It runs `eth_estimateGas`. A call that would revert
   (a bad signature, a range already anchored) gets `422` with the reason,
   and nothing is charged.
3. **The price.** A `402` quotes the gas plus a quarter of headroom at the
   current fee (base plus tip), plus `--margin` (20%), converted at
   `--eth-usd`, and at least `--min-fee` units. The quote holds for two
   minutes, so the paid retry pays exactly what was quoted.
4. **Paid:** the payment is verified, the transaction submitted, and the
   relay waits for its receipt. The payment is settled only once the
   transaction succeeded. A call that reverts on chain after all, for
   example because another got in first, gets `409` and isn't charged.
   Success gets `200 {tx, block, gasUsed, call}` with `PAYMENT-RESPONSE`.

`GET /status`: the relay's address and ETH balance, the chain, the factory,
the calls it takes and the prices.

## Trust

The relay is trusted for liveness only. The contracts check every
signature, so a relay can't forge a call, change one, or submit one for a
wallet that didn't sign it. It can delay or drop one. A lease signature
carries a deadline (VERSION 10, #171), so a claim it holds back can't be used
later to take a lease away from a live instance. A node that sees its anchor
not land falls back to another relay, or to its own ETH.

The relay takes the risk the other way round: it pays the gas before it
settles the payment. A payer who empties their wallet in between gets one
call for free. Settling first would put that risk on the node instead, for
a transaction that might never land.

## Cost

On HPP Sepolia (0.01 gwei), an anchor costs about 73.5k gas, or $0.0022
of ETH at $3,000. The relay quotes the estimate with its headroom and
margin, about 0.0037 USDC.e per anchor, or about $5 a month at one anchor
per 30 minutes. Creating a rollup costs 3.43M gas, a quote of about
0.15 USDC.e. The headroom is charged even when the transaction uses less;
a relay that wants to charge what was used would settle that amount instead
(#172 is the pricing work).

## Paid by voucher (#179)

Each 402 also offers x402's `batch-settlement`, through
`operators/x402seller/channels.py` (`--no-channels` turns it off). A node with
`relaydeposit=` deposits once into a channel in x402's escrow and pays each
call with a signed voucher. The relay charges the gas the call actually took
(`gasUsed` at its effective price, converted at `--eth-usd` with the margin),
within the quote the 402 named, and only once the call has landed. That is
the scheme's usage pricing.

The relay's key is the channel's receiver-authorizer: one `claimWithSignature`
per `--claim-interval` for all channels, then `settle` to `--pay-to`. The
book is kept in `--dir`. A wallet with no ETH gets its unspent deposit back
only by the relay's refund of idle channels (`--refund-idle`). Anvil scenario
31 runs a zero-ETH wallet through it: one deposit, no transfer per call, and
`relay_spent` equal to the relay's book.

## Tests

`selftest.sh` (ctest `test_relay`) runs on a fresh anvil with the factory
and MockUSDC:
- A new wallet holding 10 MockUSDC and no ETH creates its rollup, claims the
  lease, anchors heights 1-3, renews and releases, all through the relay.
  Each call is paid on inclusion.
- A range anchored twice, a bad signature, a token transfer and an owner
  call without a signed form are each refused, and nothing is charged.
- The wallet's ETH balance is zero at the end.
