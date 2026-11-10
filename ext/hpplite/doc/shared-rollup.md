# The shared rollup (draft for decision, not built)

Status: a draft (#297), 2026-11-10, written for the maintainer's morning decision at
somaro's request (somaro decision 0016, docs/FLEET.md). Nothing here is built
or deployed. It answers: per-database leases with epoch fencing, the epoch
root and its manifest, how `dbname=` maps onto it, whether page roots and
`hpplite-verify` still hold, and migrating a dedicated rollup into a shared one.

## The problems it answers (measured by somaro on HPP Sepolia)

1. Each database is a rollup: $0.105 of owner gas at creation (createRollup
   3.4M gas + setSequencer + setLeaseDuration), for every app including the
   ones that sleep forever. A deploy-and-rm loop spends without bound.
2. Each database anchors on its own: one transaction per database per
   anchor interval, 75k gas each.
3. Nothing on the chain answers "which databases does this platform have?";
   opening by `dbname=` scans the wallet's rollups (O(n) per open, measured
   in anvil scenario 41).

Already done without the shared rollup (2026-11-10): one process per host for
many databases (`hpplite-sequencer --dir`, #291), idle databases asking the
chain nothing (#292), one store write per batch (#296), and the operators at
cost on Sepolia (#293-#295, sepolia-v0.5.0). What is left is per-database
creation and anchoring on the chain, and discovery.

## Shape

- **Shared rollup**: one contract per fleet (a platform, or the common fleet an
  anchor service runs). It keeps, per database: `dbId`, `owner`,
  `sequencer` (the database's own key), the lease `{instance, until, epoch}`.
- **Fleet sequencer**: `hpplite-sequencer --dir` (#291) holds every
  database of a host, each with its own key; nothing about it is shared but
  the process.
- **Epoch root**: every anchor interval one transaction for the whole fleet,
  `postRoot(epochRootSeq, root, manifestDigest)`.
- **Dedicated rollup**: today's per-database rollup, kept as the opt-in
  promotion.

### A database's head is today's Range, signed by its own key

The VERSION 10 DA contract already takes, per anchor, a message signed by the
database's sequencer key:

    Range(bytes32 instanceId, uint256 fromHeight, uint256 toHeight,
          bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest)

The shared rollup keeps that message unchanged as the database's **head**,
with the domain's `verifyingContract` the shared rollup and `dbId` added:

    Head(bytes32 dbId, bytes32 instanceId, uint256 fromHeight, uint256 toHeight,
         bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest, uint256 epoch)

A fleet sequencer cannot forge another database's head: each is signed by
that database's key, which the poster of the root does not hold.

### The epoch root and its manifest

- Leaves: `keccak(dbId, owner, nameHash, headDigest)` for every database whose
  head moved since the last root (a database that wrote nothing is not in the
  root and costs nothing).
- The **manifest**: the list of leaves with the heads and their signatures,
  put at the store under the fleet's name, addressed by its sha-256 - the same
  way a range's manifest is today.
- The transaction: `postRoot(seq, root, manifestDigest, receipt)`, sent by the
  fleet's anchor key (or the relay). Gas: one write of the root and the
  digest, ~50k, whatever the number of databases. The store's bucket
  statement for the manifest rides along as today (`receipt`).
- The contract checks nothing per database at root time - it cannot afford
  to with many leaves. Checking moves to the verifier (below), with one
  exception: the lease epoch, which the contract owns.

### Leases with epoch fencing

- `claim(dbId, instance, deadline, sig)` signed by the database's sequencer
  key: sets the lease and increments `epoch[dbId]`. `forceExpire(dbId)` by
  the owner also increments it.
- The store fences on the epoch, not on the instance the rollup names
  (today's check, #167): a name put carries `(dbId, epoch)` and the store
  refuses an epoch older than the shared rollup's. A sequencer that lost its
  lease cannot add history, even before the next root.
- A head signed at epoch `e` is accepted by the verifier only if `e` was the
  database's epoch when the root covering it was posted.
- Cost: one claim transaction per database per lease (days with somaro#67's
  7-day leases) - the one per-database transaction left, and sponsored.

### `dbname=` and discovery

- `dbId = keccak(owner, name)`: computed by the node, no scan.
- Open: `fleet=<shared rollup>&dbname=<name>` (or `dbid=`). The node reads the
  database's record (one call) and its latest head from the last root's
  manifest.
- Registration: the first claim of a `dbId` creates its record (owner,
  sequencer, nameHash) - no separate creation transaction. Gas: the claim's
  first-write slots, ~60k, against $0.105 today.
- "Which databases does this platform have?": the records, or the last
  manifest. Answers somaro's case 6 without somaro.db.

### Verification

`hpplite-verify` for a database in a shared rollup:

1. The last root (or the one at `--to`/`--at`) and its manifest from the store;
   the manifest's sha-256 against the root's digest.
2. The database's leaf: its head, the head's signature by the database's
   `sequencer` recorded on chain, its epoch equal to the epoch at that root.
3. The rebuild from the store's names up to the head's `toHeight`, as today:
   the chain hash against `chainHashAfter`, the content against `stateRoot`.
   Page roots are per database and unchanged; the soft tier is unchanged (past
   the head instead of past the anchor).

What changes against a dedicated rollup: the anchor is a leaf instead of a
transaction, so the chain guarantees order and availability of the root, and
the database's own key guarantees its head. What a poster can do: leave a
database's head out of a root (delay), not change it. A database's owner
who distrusts the poster can promote to a dedicated rollup.

### Migration

- Dedicated → shared: the database's final Range on its dedicated rollup;
  then a first claim in the shared rollup whose record names the dedicated
  rollup and that Range as genesis (`fromHeight` continues). The dedicated
  rollup's owner sets `migratedTo(shared, dbId)` so a verifier follows.
- Shared → dedicated (promotion): a dedicated rollup created with the last
  head as genesis; the shared record marks `promotedTo`.
- Either way the database's history and its store names do not move.

## somaro's four questions

**1. What one fleet root commits to, and how an owner checks their own database
without the others' data.** A root is a Merkle root over leaves
`keccak(dbId, owner, nameHash, headDigest)`, one for each database whose head
moved since the previous root; `headDigest` is the EIP-712 digest of that
database's `Head` (heights, chain hash after, state root, manifest digest,
epoch), signed by the database's own key. An owner needs: the root's
transaction (from the chain), their leaf's head and signature, and the Merkle
path from the leaf to the root - log2(n) hashes, 32 bytes each, about 320
bytes for 1,000 databases. The manifest at the store lists every leaf (hashes
and heads, no database's content, names only as hashes), so a proof can be cut
from it by anyone; the store can also serve one path per database. Then the
owner rebuilds their own database from their own names at the store, as
today, and compares chain hash and state root with their head. Nothing of the
other databases is read beyond the sibling hashes.

**2. What a database pays when the anchor is shared.** One `postRoot` costs
about 50-60k gas whatever the number of databases (a root, a digest, the
store's receipt), against 75k per database per anchor today. At cost, a
root's gas (the receipt's `gasUsed` at its `effectiveGasPrice`, converted as
the relay converts it, #294/#295) is split equally across the k databases in
that root: a database that wrote nothing since the last root is not in it
and pays nothing. Example at today's Sepolia numbers: 55k gas at 0.01 gwei and
$2,065 is about $0.0011 a root; at k = 100 that is $0.000011 per database per
anchor. The split is the fleet operator's bookkeeping (somaro's console) - the
relay charges the root's sponsor one call; equal shares, not by bytes,
because the cost of a root does not depend on what each database wrote.

**3. Joining and leaving a fleet, and the history a verifier sees.** A
database that began with a dedicated rollup joins with its final Range there
(heights 1..h0, chain hash C0, state root R0): its first claim in the shared
rollup records `genesis = (dedicated rollup, h0, C0, R0)`, and the dedicated
rollup's owner sets `migratedTo(shared, dbId)`. A verifier rebuilds 1..h0
from the dedicated rollup's ranges and names, as today, then h0+1.. from the
shared heads, checking that the first shared head starts at h0+1 and that the
rolling hash continues from C0 - one unbroken chain hash across the move.
The names move with the contract: before the move they are `<old da>-<h>`,
after it `<shared da>-<dbId>-<h>` (a name format for the shared rollup); the
old names stay readable, nothing is copied. Leaving (promotion) is the
reverse: a dedicated rollup created with the last shared head as genesis,
the shared record marked `promotedTo`.

**4. What is lost when the fleet's host is lost between two roots.** Nothing
that reached the store: every batch is at the store under its name, signed by
its database's key, the moment it is flushed (#296: its one write). A
successor - after the owner's `forceExpire`, which moves the epoch - recovers
them from the store as today (#167: 'recovered batches 1-5 from the store')
and the next root covers them. What is lost is what was never flushed: the
commits of the last interval (somaro: 30 s) still in the dead host's file -
the same window as today with one process per database. Between flush and
root, a batch is soft: held by the store and signed by its sequencer, not yet
on the chain - for up to one root interval (somaro: 30 min, or 24 h with
anchor=24h). The epoch fences a lost host that comes back: its old epoch's
names are refused by the store.

## Names

"Shared rollup" and "dedicated rollup": agreed. For the third, "anchor
operator" fits the vocabulary already in use (store operator, relay
operator); "anchor service" is fine too. "Shared sequencer" avoided: nothing
sequences across databases.

## Open for the decision

1. Lease on chain per database (above), or the store as the lease
   authority with only roots on chain (cheaper, but a store could then fence
   wrongly; the roots would still expose a fork after the fact).
2. Root interval: somaro's anchor interval (30 min) or longer for idle fleets.
3. Who may post roots: the fleet's anchor key only, or any key with a
   relay-sponsored call.
4. A new factory VERSION (11) with the shared rollup beside the dedicated
   one; v10 rollups keep working.

## Cost of building it (rough)

Contract and tests (Solidity, anvil scenarios), node (open by `fleet=`, heads
in a manifest, fleet anchor in `hpplite-sequencer --dir`), store (epoch
fencing, the fleet's manifest name), verify, protocol documents and vectors,
the Worker. Several days of work, in steps that each ship on their own.
