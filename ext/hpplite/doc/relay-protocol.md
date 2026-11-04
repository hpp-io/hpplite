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
| `402` | the price (below) |
| `200 {tx, block, gasUsed, call}` | submitted and included successfully; `PAYMENT-RESPONSE` |
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

The `402` quotes the call's gas (the estimate plus headroom) at the current
fee, plus the relay's margin, converted to the asset's atomic units, at
least its minimum fee. Offers as in the store protocol: `exact`, and
`batch-settlement` when the relay offers channels.

Every offer's `extra.quote` is `{gas, until, mac}`: the gas the price was
computed for, the quote's expiry (unix seconds), and a MAC by the relay over
the chain, the target, the calldata, the amount, the gas and the expiry
(#184). The payer repeats the call naming the offer unchanged as `accepted`;
a relay honours a quote it made, for that call, until `until`, with nothing
remembered between the `402` and the retry. An `accepted` whose quote does
not verify is priced anew.

A payment is settled only after the transaction succeeded. Paid by voucher,
the call is charged what it cost - `gasUsed` at its effective gas price,
converted as quoted - at most the quote (`extra.chargedAmount` in
`PAYMENT-RESPONSE`).

## `GET /status`

```json
{
  "service": "hpplite-relay", "protocol": "hpplite-relay/1",
  "address": "0x…", "chainId": 181228, "factory": "0x…", "balanceWei": 0,
  "calls": ["claimSequencerFor", …],
  "payment": {"scheme": "exact", "network": "eip155:181228", "asset": "0x…", "payTo": "0x…",
              "ethUsd": 3000, "margin": 0.2, "minFee": 100, "facilitator": "https://…"}
}
```

## Trust

Liveness only. The contracts check every signature, so a relay cannot forge
or change a call; it can delay or drop one. Lease signatures carry a
deadline. A node whose write does not land falls back to its own ETH, or
waits with the reason in `last_error`.
