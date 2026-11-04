# HPPLite operators

The services a node pays over x402 instead of holding ETH or running
storage itself (#181). These are the reference implementations: standard
library Python, readable, and the fixtures ctest runs the node against.
The canonical operators run the same protocol on Cloudflare Workers.

| Directory | What |
|-----------|------|
| [store](store/README.md) | The pin service (#125): batches by CID, signed receipts, names `<da>-<height>` for the soft tier (#165), paid per put or by voucher |
| [relay](relay/README.md) | The relay (#170): submits the node's signed contract calls and pays their gas |
| `x402seller/` | Shared by both: `eth.py` (keccak, secp256k1, EIP-712), `payments.py` (the 402, verify, settle), `channels.py` (batch-settlement, #135) |
| `conformance/` | `buyer.py`, the buyer's side in Python: what the node does in C, used by the selftests |

The protocols they speak, and what a node expects of any implementation:
[doc/store-protocol.md](../doc/store-protocol.md) (`hpplite-store/1`) and
[doc/relay-protocol.md](../doc/relay-protocol.md) (`hpplite-relay/1`), #188.

The store and the relay are deployed separately: the relay holds a funded
hot key and sends transactions, the store holds data.
| `conformance/facilitator_mock.py` | An x402 facilitator for anvil (#183): both schemes, its own key, HPP's facilitator's habits. The services only ever talk to a facilitator URL |
