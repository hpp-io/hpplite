# Reference relay (#170)

`relay.py` submits an HPPLite node's signed contract calls and pays their
gas, charged over x402 in USDC.e. With it, a node whose wallet holds only
USDC.e can create its rollup, take and renew its lease, and anchor. With the
pin service beside it ([../store](../store/README.md)), it needs no ETH
at all. Standard library Python. It shares `../x402seller` with the store:
`eth.py` (keccak, secp256k1, EIP-712) and the x402 seller code.

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
3. **The price.** A `402` quotes the estimated gas at the current fee
   (base plus tip), plus `--margin` (20%), converted at `--eth-usd`, and at
   least `--min-fee` units; the transaction carries the estimate plus a
   quarter and 10,000 as its gas limit, which is not charged (#293). The quote holds for two
   minutes, so the paid retry pays exactly what was quoted. The quote is
   carried in the offer itself (`extra.quote`: the gas, the expiry and a MAC
   under a key derived from the relay's), and the paid retry names it back
   in `accepted`, so the relay remembers nothing between the two: a restart,
   or another process with the same key (a Worker isolate), honours it
   (#184). A quote changed by the buyer does not verify, and is priced anew.
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
of ETH at $3,000. The relay quotes the estimate with its margin, about
0.0027 USDC.e per anchor. Creating a rollup costs 3.43M gas, a quote of
about 0.12 USDC.e. A caller paying by x402 pays its quote (`exact` settles
the signed amount); a sponsor's account pays what the call used (#294).
The canonical relay charges no margin and converts at Chainlink's ETH/USD
(#295).

## Paid per call

Every call is paid by x402's `exact` scheme at the quoted amount. The
`batch-settlement` channels of #179 (one deposit, a voucher per call, a claim
cron) were removed in v0.8 (#251): no payer used them, and `exact` at every
relayed call is the visible x402 use on HPP.

## Accounts and sponsorship (#276)

A platform that runs many apps pays for their rollups from one treasury, so
no app key holds money. The treasury gives the relay an EIP-2612 permit and
names the rollups it pays for; a call on a sponsored rollup is answered
`200` with no `402`, and the node needs no setting for it.

```sh
sponsor.py http://relay:8430 --key-file treasury.key permit 50000000
sponsor.py http://relay:8430 --key-file treasury.key --rollup-key-file owner.key sponsor 0x<rollup>
sponsor.py http://relay:8430 --key-file treasury.key cap 0x<rollup> 200      # calls a day
sponsor.py http://relay:8430 --key-file treasury.key account
sponsor.py http://relay:8430 sponsor-of 0x<rollup>
```

- **The rollup's side agrees.** A `Sponsor` carries the funder's signature
  and one by the rollup's owner or sequencer, unless the funder is one of
  them. A funder alone gets `401`: otherwise anybody could take a rollup's
  sponsorship over with an empty account.
- **Pulled before the call.** The relay pulls from the treasury with its
  own key (`permit` when the allowance is short, then `transferFrom`), at
  least `--min-pull` (1 USDC.e) at a time, and keeps the rest as the
  account's credit. A call is charged before it is submitted and refunded
  when it does not land.
- **When the sponsor cannot pay** (no permit, no balance, the cap reached)
  the caller gets the usual `402` with `sponsor: {funder, why}`, and may
  pay it itself. The relay never fronts a call.
- The book is JSON files under `--dir`. `x402` top-ups go to
  `POST /account/<funder>/pay?amount=`.

Unlike the per-call path, the money is in before the gas is spent, so the
relay carries no settlement risk on a sponsored call.

## Tests

`selftest.sh` (ctest `test_relay`) runs on a fresh anvil with the factory
and MockUSDC:
- A new wallet holding 10 MockUSDC and no ETH creates its rollup, claims the
  lease, anchors heights 1-3, renews and releases, all through the relay.
  Each call is paid on inclusion.
- A range anchored twice, a bad signature, a token transfer and an owner
  call without a signed form are each refused, and nothing is charged.
- The wallet's ETH balance is zero at the end.
- Sponsorship (#276): a treasury's permit, a `Sponsor` refused without the
  rollup's side and taken with the owner's signature beside it, a
  stranger's takeover refused; sponsored calls on the rollup and its DA
  answered `200` with the caller paying nothing, the first pulling
  `--min-pull`, the next paid from credit; a cap, the capped `402` and a
  caller paying it itself; a release; an x402 top-up. The treasury holds
  no ETH at the end.
