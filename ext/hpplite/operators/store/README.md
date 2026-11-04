# A reference pin service for HPPLite (#125)

What a storage provider for HPPLite nodes looks like, in one Python file
with no dependencies beyond the standard library (`../x402seller/eth.py` does
keccak-256, secp256k1 and EIP-712 in pure Python; slow, and far faster than
a pin service needs). An HPP operator reads it before running the real
service; HPPLite's IPFS store adapter (#11) is tested against it.

```
pin_service.py --port 8420 --dir /var/lib/pins --signer-key /etc/pins/key \
    --pay-to 0xYourWallet --facilitator https://facilitator-sepolia.hpp.io \
    --network eip155:181228 --asset 0x401eCb1D350407f13ba348573E5630B83638E30D
```

Without `--facilitator` every call is free: the storage and receipt logic
alone, which is what `selftest.sh` and ctest's `test_pin_service` exercise.

## The contract with a node

Four calls. The node (or `../conformance/buyer.py`, the same thing in Python) does them in
this order and verifies every receipt before it believes it.

| Call | Paid | Answer |
|---|---|---|
| `POST /put` with the bytes | yes: `pricePerPin` + `pricePerMiBMonth` × size in MiB × retention in 30-day months, rounded up to a whole unit (+ `pricePerByte` × size, 0 by default), buys `retentionSeconds` (#172) | `{cid, bytes, expires, receipt, held}` |
| `POST /pin/<cid>` | yes, the same price | retention extended, a new `receipt` |
| `GET /get/<cid>` | no (`--paid-get` to charge) | the bytes |
| `POST /name/<da>-<height>` with the bytes, then the sequencer instance (32 bytes) and its 65-byte EIP-712 `Batch` signature appended (#165) | no | `{name, bytes, signer}`; 403 when another signer holds the rollup's names, or (with `--rpc`) when the signer is not the coordinator's `sequencerWallet()`; 409 (with `--rpc`) when the instance is not the one the lease names (`sequencerInstance()`): a sequencer that lost the lease, partitioned with the same key, can't write over its successor's names (#167); the same bytes again: 200 |
| `GET /name/<da>-<height>` | no | the bytes with the instance and the signature appended; `X-HPPLite-Signer` the wallet it recovers to |
| `GET /status` | no | prices, retention, the **signer** and the receipt's typed-data layout |

- **Idempotent by CID.** A `put` of bytes already held answers 200 with the
  existing receipt and no 402. A node whose payment settled while the
  upload failed retries and pays nothing twice.
- **The CID is CIDv1, raw codec, sha-256, base32** (`bafkrei…`). The same
  bytes give the same CID at every IPFS provider, so one commitment on
  chain can be pinned at several (`--kubo http://127.0.0.1:5001` pins on a
  Kubo node too and refuses to answer a CID Kubo would compute differently).
- **The receipt is the provider's promise:** an EIP-712 signature by
  `--signer-key` over `Receipt(bytes32 hash, string cid, uint256 expires)`
  in the domain `("HPPLitePinService", "1", chainId, payTo)`. `/status`
  publishes the signer, as HPP's facilitator publishes its signers. A node
  verifies the receipt (`buyer.receipt_ok`), keeps it, and commits it with
  the batch (`commitBatch`, #33), so anyone can later show what the provider
  undertook to hold until when.

## What it charges (#172)

`--price-per-mib-month` (default 30 atomic units, so $0.03 per GiB-month:
R2's $0.015 plus 100%) prices the storage, with fractions allowed; each put
is rounded up to a whole unit. `--price-per-pin` (default 10 units,
$0.00001) is per put, whatever its size. `--price-per-byte` (default 0) is
the price before #172, kept for anyone who set it; the old default of 1
unit per byte made a megabyte cost $1 and a gigabyte $1,000.

At the defaults, a 2 KB batch kept 30 days costs 11 units: 10 for the put
and 1 for the storage, rounded up. A database flushing every 5 seconds
puts 17,280 batches a day, about $6 a month, nearly all of it the per-put
part. That is the real cost of a payment per put: each `exact` payment is
a settlement transaction for the facilitator. #135 (x402's
`batch-settlement`, which HPP's facilitator supports) is what removes it,
with one deposit and signed vouchers instead of a settlement per put. The
node's `store_price` quotes a put exactly as the 402 will
(`hpplite_store_quote`), so `storecap=` can be set from it.

## The x402 seller side, written out

`Handler.paid()` is the whole protocol from the seller's side, which is
why it is not behind a library:

1. no `PAYMENT-SIGNATURE` header: answer `402` with `PAYMENT-REQUIRED`, a
   base64 `PaymentRequired` naming the price (`amount`, atomic units of
   `asset`), `payTo`, the network and the token's EIP-712 domain (`extra`);
2. with one: decode the `PaymentPayload`, `POST /verify` it to the
   facilitator with the requirements;
3. valid: do the work; if the work fails nothing is settled;
4. `POST /settle`; on success answer 200 with `PAYMENT-RESPONSE`.

No signing happens in the seller. The buyer signs an EIP-3009
`TransferWithAuthorization` for exactly the quoted amount with a random
nonce; the facilitator submits it and pays the gas. `hpp-x402 serve
--handler <url>` wraps a plain endpoint in the same dance with no code.

On anvil the facilitator is `../conformance/facilitator_mock.py` (#183),
which `tests/l1/anvil_test.sh` starts with a key of its own: verify is an
`eth_call` of `transferWithAuthorization` (the token itself checks the
signature), settle sends the same call; for batch-settlement it sends the
escrow's deposit, claim, settle and refund, answers `/verify` with the
channel's state, and can answer a deposit `settlement_queued` and repeat an
earlier `/settle` answer, as HPP's does. The service never sends a
transaction itself.
`contracts/src/test/MockUSDC.sol` is the token, EIP-3009 with the real
token's domain, `("Bridged USDC", "2")` (its `name()`; the symbol is USDC.e,
and HPP's facilitator checks the quoted name against the token). That is anvil scenario 18 (`tests/l1/test_anvil.c`):
the buyer pays a put and a pin, is refused over its cap before signing,
and the pay-to wallet ends richer by what settled.

## batch-settlement: a deposit once, a voucher per put (#135)

Each 402 offers x402's `batch-settlement` next to `exact` (`--no-channels`
turns it off). A node that takes it deposits once into a channel in x402's
escrow, `x402BatchSettlement` at `0x4020074e9dF2ce1deE5A9C1b5c3f541D02a10003`
on every chain. Its first payment authorizes an ERC-3009
`receiveWithAuthorization` to the deposit collector, and the facilitator
sends it. Every put after that carries a signed `Voucher(channelId,
maxClaimableAmount)` for the node's running total. `channels.py` checks the
voucher itself, with nothing on the chain:
- the signature recovers to the channel's `payerAuthorizer`;
- the total is exactly what it has charged plus this put's price;
- the total is within the deposit.

A total that doesn't match gets the corrective 402. It carries the channel's
state and the node's own last voucher, which the node checks before adopting
the total. A total past the balance is refused, and the node tops up with
another deposit.

Every `--claim-interval` (default 1 hour), one `claimWithSignature`
transaction claims all channels charged since the last claim, then `settle`
moves the money to `--pay-to`. The service signs as `receiverAuthorizer`
with its receipt key. A channel idle for `--refund-idle` (default 7 days)
gets back what it holds beyond its charges. All of it runs in
`Channels.tick()`: the reference calls it from a timer thread, a Worker
from a Cron Trigger (#185).

The refund matters for a node with no ETH. The escrow's own exit, a timed
withdrawal after `--withdraw-delay` (default 1 day), must be sent by the
payer, so it costs gas. The claim interval has to stay well inside the
withdraw delay, because vouchers not claimed when a withdrawal finishes are
lost to the service.

Two behaviours of HPP's facilitator are handled here:
- **A deposit answered `settlement_queued`** (past the facilitator's
  synchronous window) is being sent, not refused. The service records it as
  pending and answers at once: a `402` with
  `invalid_batch_settlement_evm_deposit_pending`, nothing waiting on the
  chain (#185). The node's next attempt finds it landed, and the voucher is
  taken with no second deposit; while it hasn't landed (up to 10 minutes) no
  other deposit is sent for that channel. The service also reads the channel
  before sending any deposit, and doesn't send one the channel no longer
  needs.
- **The facilitator answers a `/settle` body it has already seen** with its
  earlier result. `settle(receiver, token)` would be the same body every time,
  so each request carries `extra.requestedAt`.

The per-put fee (`--price-per-pin`) is unchanged. With vouchers it no longer
pays for a settlement transaction, so an operator can lower it.

`channel_selftest.sh` (ctest `test_pin_service_channels`) runs this on anvil
against the escrow's bytecode as HPP Sepolia runs it
(`tests/l1/fixtures/`). A buyer holding MockUSDC and no ETH does the
following:
- one deposit for many puts, with no transaction for them;
- the corrective 402;
- a voucher from another key, refused;
- one claim that pays the service exactly what it charged;
- the service losing its book and taking the chain's claimed total as the
  baseline;
- a top-up;
- an idle refund that gives the buyer back everything not charged.

## Against HPP Sepolia

The real facilitator is `https://facilitator-sepolia.hpp.io`, the asset
USDC.e (six decimals) at the address above, the network `eip155:181228`.
The buyer needs USDC.e; `hpp-x402 fund` shows where to send it. Then:

```
../conformance/buyer.py --service http://host:8420 --key <hex> --cap 50000 put batch.bin
../conformance/buyer.py --service http://host:8420 --key <hex> status
```

## Not here, on purpose

Per-byte settlement with the `upto` scheme and batch-settlement channels
for per-flush volume (HPP's facilitator supports both); a `/commit` relay
endpoint that submits a signed commitment to the rollup for a node that
holds no ETH (#126); a bond and a challenge contract that turn the receipt
into an enforceable promise. Each is a later layer on the four calls above.
