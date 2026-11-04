# The store protocol, `hpplite-store/1`

What an HPPLite node expects of a store given as `store=http(s)://host:port`
(#188). The reference implementation is
[operators/store](../operators/store/README.md); the canonical store runs the
same protocol on Cloudflare Workers (#190). Both are checked by the same
conformance suite (#189).

A service says which protocol it speaks in `GET /status` (`"protocol":
"hpplite-store/1"`). A node puts nothing to a service that names a version it
does not know, or another protocol (a relay given as `store=`): the batch
waits and `hpplite_status.last_error` says why. A service that names none is
taken as version 1 (it predates the field). A new version is made when a
change would make an older node or an older service misread the other;
fields added to an answer are not a new version.

## Calls

| Call | Paid | Answer |
|------|------|--------|
| `POST /put`, the body the bytes | yes (x402) | `200 {cid, bytes, expires, receipt, held}` |
| `POST /pin/<cid>` | yes, as a put of that size | `200 {cid, bytes, expires, receipt}`: retention extended |
| `GET /get/<cid>` | no, unless the service says `paidGet` | `200` the bytes |
| `POST /name/<da>-<height>`, the body the bytes, the instance (32 bytes) and the signature (65 bytes) | no | `200 {name, bytes, signer}` |
| `GET /name/<da>-<height>` | no | `200` the body as put; `X-HPPLite-Signer` |
| `GET /status` | no | below |

Errors: `400` a malformed request (an empty put, a name that is not
`<40 hex>-<decimal>`, a body too short to carry a signature); `402` payment
required or refused (below); `403` a name refused (another signer holds the
rollup's names, or with a chain, the signer is not the rollup's sequencer);
`404` not held (never put, or past its retention); `409` a name from an
instance the rollup's lease does not name; `413` over the service's size
limit (16 MiB in the reference). Error
bodies are JSON with `error`.

### Content

- **The CID** is CIDv1, raw codec (0x55), sha2-256, multibase base32 lower
  (`bafkrei…`): the same bytes give the same CID anywhere.
- **Idempotent by CID.** A put of bytes already held answers `200` with the
  existing receipt and `held: true`, and asks nothing.
- **The receipt** is the store's promise to hold the bytes until `expires`:
  `{hash, cid, expires, signer, signature}`, an EIP-712 signature over
  `Receipt(bytes32 hash,string cid,uint256 expires)` where `hash` is the
  keccak-256 of the bytes, in the domain `("HPPLitePinService", "1", chainId,
  payTo)` as `/status` gives it. The node checks it recovers to `/status`'s
  `signer` and commits it with the batch.
- **Retention.** A put buys `retentionSeconds`; `/pin` extends from the later
  of now and the current expiry. Past it the bytes may be deleted.

### Names (the soft tier, #165)

`<da>-<height>`: the rollup's DA contract (40 hex, lower case) and the batch
height. The body is the batch's bytes followed by the sequencer's instance id
(32 bytes) and its EIP-712 signature (65 bytes, r‖s‖v) over
`Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)` in the DA
contract's domain `("HPPLiteDA", "3", chainId, da)`, `batchHash` the
keccak-256 of the bytes.

- The first signer to put a name for a DA holds that DA's names; another
  signer is refused `403`. The decision must be atomic across everything that
  serves the store (a conditional create), and reads must see a name as soon
  as its put answered: a replica follows the names as they are written.
- A store that reads the chain also refuses a signer that is not the
  rollup's `sequencerWallet()` (`403`), and an instance other than the
  lease's `sequencerInstance()` (`409`, #167).
- The same body again answers `200`.
- `X-HPPLite-Signer` on a read is the wallet the signature recovers to.
- Names are kept for the store's `retentionSeconds` from their put.

## `GET /status`

```json
{
  "service": "hpplite-pin-service", "protocol": "hpplite-store/1",
  "signer": "0x…",
  "receipt": {"domain": {"name": "HPPLitePinService", "version": "1", "chainId": 181228, "verifyingContract": "0x…"},
              "type": "Receipt(bytes32 hash,string cid,uint256 expires)"},
  "payment": {"scheme": "exact", "schemes": ["exact", "batch-settlement"], "network": "eip155:181228",
              "asset": "0x…", "payTo": "0x…", "pricePerPin": 10, "pricePerMiBMonth": 30.0, "pricePerByte": 0,
              "retentionSeconds": 2592000, "paidGet": false, "facilitator": "https://…", "channels": {…}}
}
```

`payment` is `null` for a free service. The node reads `signer` and the
receipt's domain to verify receipts, and the prices to quote a put before it
is asked (`hpplite_status.store_price`).

## Payment (x402 v2)

A paid call without `PAYMENT-SIGNATURE` answers `402` with the requirements
in the body and, base64, in `PAYMENT-REQUIRED`. The price of a put of `n`
bytes is `pricePerPin + pricePerByte × n + ceil(pricePerMiBMonth × n / 2^20 ×
retentionSeconds / 2592000 − 10^-9)`, in the asset's atomic units (USDC.e:
10^-6), computed in IEEE doubles in that order (the node quotes it the same
way; `operators/conformance/vectors.json` has cases).

`accepts` offers:

- **`exact`**: an EIP-3009 `TransferWithAuthorization` for the amount, to
  `payTo`, settled by the facilitator after the work is done. `extra` names
  the token's EIP-712 domain (`name`, `version`).
- **`batch-settlement`** (when the service offers channels, #135): x402's
  escrow at `0x4020074e9dF2ce1deE5A9C1b5c3f541D02a10003`; `extra` adds
  `receiverAuthorizer`, `withdrawDelay` and `minDeposit`. The payer sends a
  cumulative voucher per call, with a deposit when the channel cannot cover
  it.

The payer repeats the call with `PAYMENT-SIGNATURE` (base64 JSON:
`x402Version`, `accepted` - the offer it chose, unchanged - and `payload`).
On success the answer carries `PAYMENT-RESPONSE`; for a voucher its `extra`
has `chargedAmount` and `channelState`.

A `402` to a paid request names the reason in `error`. The node acts on:

| `error` | Meaning | The node |
|---------|---------|----------|
| `invalid_batch_settlement_evm_cumulative_amount_mismatch` | its running total differs from the service's; `extra.channelState` and `extra.voucherState` (the node's own last voucher) say where it stands | adopts the total if the voucher is its own, and pays again |
| `…cumulative_exceeds_balance`, `…channel_not_found` | the channel cannot cover it | pays again with a deposit |
| `invalid_batch_settlement_evm_deposit_pending` | the facilitator queued the deposit (#185); no other deposit is sent for the channel while it is pending | tries again later; the voucher is taken once the deposit lands |

Nothing done is charged: a call that fails after a payment verified is not
settled, and a voucher counts only once its call answered `200`.
