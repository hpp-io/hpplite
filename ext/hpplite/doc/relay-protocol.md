# The relay protocol, `hpplite-relay/1`

What an HPPLite node expects of a relay given as `relay=http(s)://host:port`
(#188): a service that submits the node's EIP-712 signed contract calls and
pays their gas, charged over x402, so a wallet with no ETH runs (#126). The
reference implementation is [operators/relay](../operators/relay/README.md);
the canonical relay runs the same protocol on Cloudflare Workers (#191).

`GET /status` names the protocol (`"protocol": "hpplite-relay/1"`). A node
sends nothing to a relay naming a version it does not know, or another
protocol (a store given as `relay=`); a relay naming none is taken as
version 1. Versions change as in [the store protocol](store-protocol.md).

## Calls

`POST /commit` with `{"to": address, "data": calldata}`, and `GET /status`.

| Answer | When |
|--------|------|
| `400` | the body is not `{to, data}`, or not a call the relay takes (below); nothing simulated |
| `422 {error, call}` | the call would revert (`eth_estimateGas`); `error` carries the revert reason. Nothing charged |
| `402` | the price (below); on a sponsored rollup whose sponsor cannot pay, also `sponsor: {funder, why}` |
| `200 {tx, block, gasUsed, call}` | submitted and included successfully; `PAYMENT-RESPONSE`. On a sponsored rollup (below) also `sponsor` (the funder) and `charged` (what its account paid), and no payment was asked |
| `409 {error, tx, call, charged: false}` | submitted and reverted on chain (another call got in first), or no receipt within the relay's timeout (`tx` names it). Not charged |
| `502 {error, tx: null, call, charged: false}` | the chain refused the transaction. Not charged |

### The calls taken

Only the signed forms (`…For`), only on the factory the relay serves and on
the rollups and DA contracts that factory created. For factory VERSION 10:

- factory: `createRollupFor(address,uint256,uint256,uint256,bytes)`;
- rollup: `claimSequencerFor`, `renewLeaseFor`, `releaseLeaseFor`
  `(bytes32,uint256,uint256,bytes)`, `submitCheckpointFor(bytes32,uint256,uint256,bytes32,bytes)`,
  `setCheckpointIntervalFor(uint256,uint256,uint256,bytes)`;
- DA: `commitRangeFor(bytes32,uint256,uint256,bytes32,bytes32,bytes32,bytes,bytes)`,
  `submitBatchFor(bytes32,uint256,bytes,bytes)`,
  `commitBatchFor(bytes32,uint256,bytes32,bytes,bytes,bytes)`.

`/status` lists them by name in `calls`. A factory version that adds or
changes a signed form changes this list; a relay serves one factory.

## The price and the quote

The `402` quotes the call's estimated gas at the current fee, plus the
relay's margin, converted to the asset's atomic units, at least its minimum
fee: `max(ceil(gas × fee × (1 + margin) × ethUsd × 10^6 / 10^18), minFee)`
in IEEE doubles in that order (cases in `operators/conformance/vectors.json`).
`gas` is the estimate itself; the headroom a transaction needs (the
estimate × 1.25 + 10,000 in both relays) is its gas limit, not charged
(#293). The offer is x402's `exact` (the
`batch-settlement` channels of v0.5-v0.7 were removed in v0.8, #251).

Every offer's `extra.quote` is `{gas, until, mac}`: the gas limit the
transaction will carry, the quote's expiry (unix seconds), and a MAC by the relay over
the chain, the target, the calldata, the amount, the gas and the expiry
(#184). The payer repeats the call naming the offer unchanged as `accepted`;
a relay honours a quote it made, for that call, until `until`, with nothing
remembered between the `402` and the retry. An `accepted` whose quote does
not verify is priced anew.

A payment is settled only after the transaction succeeded, at the quoted
amount.

## Accounts and sponsorship

A funder - a platform's treasury - pays the relay for the rollups it names
(#276), so a rollup's sequencer key holds nothing and sends no payment. A
call on a sponsored rollup sent without `PAYMENT-SIGNATURE` is submitted and
answered `200` with `sponsor` and `charged`, the price taken from the
sponsor's account. A rollup nobody sponsors, or whose sponsor cannot pay,
gets the `402` above as before (with `sponsor: {funder, why}` when there is
a sponsor), and a caller that sends a payment always pays itself. A node
needs nothing for this: it takes a `200` without a `402` as a free relay
gives one. A DA contract's calls are sponsored through the rollup that is
its coordinator. The factory's `createRollupFor` has no rollup yet and is
never sponsored. A free relay (no `payment` in `/status`) keeps no accounts:
`accounts` is `null` and these routes answer `404`.

The messages are EIP-712 in the relay's domain `("HPPLiteRelay", "1",
chainId, payTo)` (`/status` → `accounts.domain`):

| Type | For |
|------|-----|
| `Sponsor(address rollup,address funder,uint256 issuedAt)` | the funder pays for this rollup's calls and its DA's |
| `Unsponsor(address rollup,address funder,uint256 issuedAt)` | the sponsor stops paying for it |
| `Cap(address rollup,uint256 callsPerDay,uint256 issuedAt)` | at most this many sponsored calls in a rolling day (0: no bound); the cap goes when the sponsor changes |

| Call | Signed by | Answer |
|------|-----------|--------|
| `POST /account/<funder>/permit`, body `{value, deadline, signature}` (`{v, r, s}` taken too) | the funder: an EIP-2612 permit of the asset to `accounts.spender`, at the token's current nonce | `200` the account; `401` it does not recover to the funder |
| `POST /account/<funder>/pay?amount=<units>` | anyone, by x402 `exact` (at least `accounts.minPull`) | `402` the offer; paid: `200` the account, `credit` raised; `400` below `minPull` |
| `POST /sponsor/<rollup>`, body `{funder, issuedAt, signature, rollupSignature?}` | the funder, and the rollup's side (below) | `200` the sponsorship; `401` either signature; `404` not a rollup of the factory; `409` `issuedAt` not newer than the last taken; `503` the chain cannot be read |
| `POST /sponsor/<rollup>/release`, body `{funder, issuedAt, signature}` | the sponsor | `200` the sponsorship, `state: released`; `401` not the sponsor |
| `POST /sponsor/<rollup>/cap`, body `{funder, callsPerDay, issuedAt, signature}` | the sponsor | `200` the sponsorship; `401` not the sponsor; `409` not newer |
| `GET /account/<funder>` | - | `200 {funder, credit, balance, allowance, permitNonce, permit, rollups, state, why}`; `404` |
| `GET /sponsor/<rollup>` | - | `200 {rollup, da, funder, cap, today: {calls, since}, state}`; `404` |

**Who may name a rollup's sponsor.** The rollup's side agrees to every
`Sponsor`: `rollupSignature` is a signature of the same `Sponsor` digest
that recovers to the rollup's `owner()` or `sequencer()` on chain; it may be
left out when the funder is itself one of the two. A funder alone is
answered `401`. Without this anybody could take a rollup's sponsorship over
with a newer `issuedAt` and an empty account, and the rollup's calls would
stop being paid; with it, a newer `Sponsor` by another funder is a takeover
the rollup asked for. `Unsponsor` and `Cap` are the sponsor's alone.

**Paying.** A sponsored call is charged the price its caller would have
been quoted, before it is submitted, and given back when the transaction is
refused, fails to be included or reverts: two calls at once never spend the
same credit, and nothing is charged for a call that did not land. When it
lands, the account keeps the price of the gas the call used - the same
formula at the receipt's `gasUsed` and `effectiveGasPrice`, never more than
the quote - and is given back the rest; `charged` says what was kept (#294).
A caller paying by x402 pays its quote: `exact` settles the signed amount. The price
comes from the account's `credit` when that covers it. Otherwise the relay
first pulls from the funder: when the token's allowance to `spender` is
short it sends the permit it holds, then `transferFrom` to `payTo`, both by
the relay's own key and simulated first so a refusal costs no gas; it pulls
at least `minPull` and the rest stays as credit for the next calls. A pull
the chain refuses (no permit, a spent or expired one, no balance) leaves
the account `state: uncovered` with `why`, and the rollup's calls get their
`402` until a new permit or a top-up; the relay never fronts a call for an
uncovered account. The token keeps one permit nonce per wallet, whoever the
spender is: a funder whose wallet has a permit at a store that is not yet
sent cannot hold one here too - the first sent voids the other - so it
tops this account up (`/pay`) or gives the permit once the store's is on
chain (#281). A call past the rollup's cap gets the `402` with
`sponsor.why` saying so. `today` counts sponsored calls that landed, in a
rolling day from the first.

`operators/relay/sponsor.py` is a funder's side of this from a command
line.

## `GET /status`

```json
{
  "service": "hpplite-relay", "protocol": "hpplite-relay/1",
  "address": "0x…", "chainId": 181228, "factory": "0x…", "balanceWei": 0,
  "calls": ["claimSequencerFor", …],
  "payment": {"scheme": "exact", "network": "eip155:181228", "asset": "0x…", "payTo": "0x…",
              "ethUsd": 3000, "margin": 0.2, "minFee": 100, "facilitator": "https://…"},
  "accounts": {"spender": "0x…", "payTo": "0x…", "minPull": 1000000, "asset": "0x…",
               "assetDomain": {"name": "Bridged USDC", "version": "2"},
               "domain": {"name": "HPPLiteRelay", "version": "1", "chainId": 181228, "verifyingContract": "0x…"}}
}
```

## Trust

Liveness only. The contracts check every signature, so a relay cannot forge
or change a call; it can delay or drop one. Lease signatures carry a
deadline. A node whose write does not land falls back to its own ETH, or
waits with the reason in `last_error`.
