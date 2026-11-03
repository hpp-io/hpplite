# HPPLite Examples

Example programs demonstrating HPPLite features.

## Quick Start

HPPLite is built into SQLite - just use `?hpplite=on` in your URI:

```c
#include "sqlite3.h"

// Open database with HPPLite enabled (hpp-sepolia has built-in factory)
sqlite3 *db;
sqlite3_open_v2(
    "file:myapp.db?hpplite=on&l1=hpp-sepolia&privkey=0xYOUR_PRIVATE_KEY",
    &db,
    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
    NULL
);

// Use standard SQLite API - changes are batched and submitted to L1
sqlite3_exec(db, "CREATE TABLE users(id, name)", NULL, NULL, NULL);
sqlite3_exec(db, "INSERT INTO users VALUES(1, 'Alice')", NULL, NULL, NULL);

// Close - pending batch is flushed automatically
sqlite3_close(db);
```

On first open, the factory auto-deploys your rollup contract. All writes are batched and submitted to L1 for data availability.

## Building

From the `examples/` directory:

```bash
# Ensure HPPLite is built first
cd ../build
cmake .. -DHPPLITE_USE_REAL_L1=ON
make

# Build examples
cd ../examples
make
```

Or build individually:

```bash
gcc -I.. -I../../build -o basic_sequencer basic_sequencer.c \
    ../build/libhpplite.a ../build/libsqlite3.a \
    $(pkg-config --libs libsecp256k1 libcurl)
```

## Examples

### basic_sequencer.c

Basic example demonstrating:
- Opening database with `sqlite3_open_v2()` and `?hpplite=on` URI parameter
- Using standard `sqlite3_exec()` for all SQL operations
- Automatic batch submission to L1
- Transparent close via `sqlite3_close()`

```bash
export HPPLITE_PRIVATE_KEY=0x...
./basic_sequencer
```

### factory_demo.c

Factory pattern demonstration showing:
- Auto-discovery of existing rollup from wallet
- Auto-creation of rollup if none exists
- One rollup per wallet (1:1 wallet:rollup mapping)

```bash
export HPPLITE_PRIVATE_KEY=0x...
./factory_demo
```

**Note:** Requires network access to HPP Sepolia.

### pin_service/ and relay/

Two reference services for a node that pays in USDC.e over x402 instead of ETH:
[pin_service](pin_service) stores batches and signs receipts (#125), and
[relay](relay/README.md) submits the node's signed contract calls (its rollup,
the lease, anchors) and pays their gas (#170).

## URI Parameters

Open databases with configuration in the URI:

```
file:myapp.db?hpplite=on&l1=hpp-sepolia&privkey=0x...
```

| Parameter | Required | Description |
|-----------|----------|-------------|
| `hpplite` | Yes | Set to `on` to enable |
| `l1` | Yes* | Network alias (hpp-sepolia) or chain ID |
| `rpc` | No* | L1 RPC URL (auto-set from l1 alias) |
| `factory` | No* | Factory contract (auto-set from l1 alias) |
| `contract` | No | Direct rollup address (if known) |
| `privkey` | Yes | Private key for signing |
| `datadir` | No | Local data directory |
| `interval` | No | Batch interval (e.g., `5s`, `1m`) |

*Use `l1=hpp-sepolia` for simplest config - RPC and factory are built-in.

## Environment Variables

Configuration can also be set via environment (URI params take priority):

```bash
export HPPLITE_L1_RPC=https://sepolia.hpp.io
export HPPLITE_FACTORY_ADDRESS=0xbce61e56f84620bc4365e95126f82b51faed7b4e
export HPPLITE_PRIVATE_KEY=0x...
export HPPLITE_DATA_DIR=/var/lib/hpplite
export HPPLITE_BATCH_INTERVAL=60s
```

## Network Info

**HPP Sepolia** (recommended for testing):
- Chain ID: 181228
- RPC: https://sepolia.hpp.io
- Factory: `0xbce61e56f84620bc4365e95126f82b51faed7b4e`
- Alias: `hpp-sepolia` (use `l1=hpp-sepolia` for auto-config)

## Verification

After running an example, you can verify the state from L1:

```bash
# Verify your rollup state matches L1 data
hpplite verify 0xYOUR_ROLLUP_ADDRESS --rpc https://sepolia.hpp.io
```

This fetches all batches from L1, replays the SQL, and verifies state roots match.
