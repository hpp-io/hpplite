# The fleet protocol, `hpplite-fleet/1` (draft)

Status: draft, 2026-11-10 (#297, step 1: #299). The messages, the leaf, the tree and the
manifest are fixed here with vectors (`operators/conformance/vectors.json`,
section `fleet`; reference `operators/fleet/fleet.py`) so that the contract,
the node, the store, the anchor and a verifier compute one root. Nothing is
deployed. The design and its reasons are in [shared-rollup.md](shared-rollup.md);
somaro's decisions on it (decision 0016) are folded in below.

## Roles

- A **shared rollup** (factory VERSION 11, beside v10's dedicated rollups)
  keeps, per database: `dbId`, `owner`, `sequencer`, the lease
  `{instance, until, epoch}`, `epochSince` (the root sequence at which the
  current epoch began), and `retired`. It keeps the fleet's `owner`, its
  `admitter` and its `anchor` key, and per root sequence one slot,
  `keccak256(abi.encode(root, manifestDigest))`; `RootPosted(seq, root,
  manifestDigest)` carries both in full and `rootSeq` is the latest (#301).
- Each **database's sequencer** (its own process and key, one per app: the
  single-file `hpplite-sequencer`) signs its head at every flush; the head
  rides with the batch's name write - one store write per batch (#296).
- The **anchor operator** reads the signed heads at the store, builds the
  root and the manifest, and posts the root. It reads no database file and
  holds no database key; it can serve one platform or a common fleet.

## Messages

EIP-712 in the domain `("HPPLiteFleet", "1", chainId, <shared rollup>)`:

| Type | Signed by |
|------|-----------|
| `Head(bytes32 dbId,bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 chainHashAfter,bytes32 stateRoot,bytes32 manifestDigest,uint256 epoch)` | the database's sequencer, at each flush: today's `Range` with the database and its lease epoch. A batch's head has `fromHeight = toHeight` = its height and `manifestDigest` = the sha-256 of the batch's bytes (their CID's digest); the store refuses another (#315; vectors `fleet.batchHeads`) |
| `Admit(bytes32 dbId,address owner,address sequencer,uint256 issuedAt)` | the fleet's owner or its admitter: the first claim of a `dbId` in the fleet needs it, so nobody rides a fleet's roots and sponsorship uninvited |
| `Retire(bytes32 dbId,uint256 issuedAt)` | the database's owner: the record goes, at `rm` |
| `Root(uint256 seq,bytes32 root,bytes32 manifestDigest,uint256 deadline)` | the fleet's anchor key (the owner may change it); sponsored at the relay |
| `Claim(bytes32 dbId,bytes32 instanceId,uint256 nonce,uint256 deadline)` | the database's sequencer, to take or renew its lease (`claimFor`, or with the Admit in `joinFor`); `nonce` is the record's `claimNonce` - 0 for a dbId the fleet never had, carried on when a retired one is admitted again |
| `Expire(bytes32 dbId,uint256 nonce,uint256 deadline)` | the database's owner, for `expireFor` (#321): `forceExpire`'s effect, sent by anyone (a relay), so a restored host with N databases costs the owner's key N signatures and no gas; `nonce` is the record's `claimNonce`, consumed, so an Expire once sent cannot be played against a later lease |
| `Snapshot(bytes32 dbId,uint256 admittedAt,uint256 height,uint256 part,bytes32 digest)` | the database's sequencer (#314): one piece of its snapshot at `height` - part 0 the index, part `i >= 1` its `i`-th chunk; `digest` the sha-256 of the piece's bytes (vectors `fleet.snapshot`) |

In the factory's domain `("HPPLiteFleetFactory", "11", chainId, <the factory>)`:

| Type | Signed by |
|------|-----------|
| `CreateFleet(address owner,address anchor,uint64 leaseDuration,uint256 nonce,uint256 deadline)` | the fleet's owner, for `createFleetFor`; `nonce` is the factory's `createNonce(owner)` |

Every other change is a call by its role (`forceExpire`, `setSequencer`,
`promote` by the database's owner; `setRoles`, `transferOwnership` by the
fleet's). `vectors.py` refuses a `*_TYPEHASH` in either contract without a
vector.

`dbId = keccak256(abi.encode(owner, keccak256(name)))`: a database is its
owner and its name in the fleet; `dbname=` computes it, no scan. `admitFor`
and `joinFor` take `nameHash` and refuse a `dbId` that is not
`keccak256(abi.encode(owner, nameHash))`; the Admit signs `dbId` and `owner`,
which with that check fix the name (#307). The record keeps `nameHash` and
`Admitted` carries it, so a leaf's owner and name come from the chain. A
database's owner never changes inside a fleet: a new owner is a new `dbId`
(retire, admit again under the new owner; the data moves by a restore).

## The leaf and the tree

- Leaf: `keccak256(abi.encode(dbId, owner, nameHash, headDigest))`, with
  `nameHash = keccak256(name)` and `headDigest` the `Head`'s EIP-712 digest.
- A root holds one leaf per database whose head moved since the previous
  root; each `dbId` at most once. No head moved: no root.
- Order: leaves by `dbId` ascending, as unsigned 256-bit numbers.
- Pairs: `keccak256(left || right)` in that order, level by level; an odd
  last node moves up to the next level unchanged. One leaf: the root is the
  leaf.
- A path: the siblings bottom up, each with its side; a level where the node
  moved up unchanged adds none.

## The manifest

Canonical JSON (keys sorted, no spaces), its digest sha-256, kept at the store
under that CID as a range's manifest is:

    {"chainId": 181228, "leaves": [{"dbId", "head": {...}, "nameHash", "owner",
     "signature"}, ...], "protocol": "hpplite-fleet/1", "rollup": "0x…", "seq": 7}

with `leaves` ordered by `dbId`. Anyone can cut a database's path from it; it
carries no database's content and names only as hashes.

## Building a root

The fleet's anchor builds root `seq = rootSeq + 1` from three inputs, and two
builders given the same inputs build the same root (`fleet.build_root`,
vectors `fleet.builder`):

- **the records** - every database's `db(dbId)` read at one block `B`, the
  block the build starts from (owner, sequencer, nameHash, epoch, admittedAt,
  retired);
- **the heads** - the store's latest head of each database
  (`GET /fleet/<fleet>/heads`, or `GET .../db/<db>`'s `latestHead`), with the
  admission it was written under;
- **what the latest roots hold** - each database's head digest in its latest
  root (the store's `anchored`, checked by its path).

A database's leaf goes in when none of these holds, checked in this order: its
record is retired (or there is none); it has no head; the head is of another
admission than the record's; the head's epoch is not the record's epoch at `B`
(a fence raced the build: the database keeps the leaf of its latest root, and
its new head goes in the next); the head's signature does not recover to the
record's sequencer; the head is the one its latest root holds (nothing moved).
A head of an older epoch is never put over a newer one: the store takes only
the lease's epoch, and the builder only the record's at `B`. Leaves by `dbId`
ascending; the manifest and the root as above. No leaf, no root.

When: a root is built `anchor` after the last root on chain (`RootPosted`'s
block time; for a fleet's first root, after its first head) and only when the
rule gives a leaf - the rule a rollup's anchor follows (#308, #309); an idle
fleet sends nothing. The interval is the operator's (24 h by default for the
reference `hpplite-anchor`; somarod's own setting).

The reference anchor is `operators/fleet/anchor.py` (`hpplite-anchor`: `--rpc
--fleet --key-file --store`, `--relay` or `--send`, `--interval`, `--once`),
for a fleet run without somaro and for conformance. **Verifying a database**
is `operators/fleet/verify.py --rpc --fleet --store --db <dbId>`: one call
that trusts the store for nothing - the store's `anchored` only says where to
look; the root and the manifest digest come from `RootPosted(seq)` and
`rootIs` on chain, the manifest's sha-256 must be that digest, the leaf's
owner and name the database's record, and the path must lead from the leaf
rebuilt from the head to the root. It answers `{verified, why, dbId, seq,
root, manifestDigest, block, time, toHeight, head, path}`: the database's
history to `toHeight` is anchored at `block`.

## Finding a database's latest root, and its epoch there

- The store keeps, per fleet and `dbId`, the sequence of the latest root that
  holds the database's head. A node or a verifier asks it, then checks the
  answer with the leaf's path into that root on chain: the store's index is
  a hint, the path is the proof. A database asleep at root time is in no new
  root, and its latest head stays where it was.
- The record's `epochSince` says from which root sequence the current epoch
  holds. A head signed at epoch `e` is valid in root `s` when `e` was the
  database's epoch at `s`; a verifier reads `epoch` and `epochSince` (and,
  for older roots, the `EpochMoved(dbId, epoch, seq)` events) without an
  archive node.

## Caps, funding, retirement

- Caps and funding stay per database at the store: writes and bytes a day per
  `dbId`, the fleet's treasury as funder. One abusive app is bounded alone;
  the bound never becomes the fleet's.
- `Retire` removes the record, so the fleet's list of databases stays true.
  Its names stay at the store until the bucket is released or pruned, as a
  released bucket's do today; a verifier of past roots still finds the heads
  in their manifests.

## The store (step 3; somaro's decisions folded in, before code)

A fleet's databases write to a store that speaks `hpplite-store/2` plus the
calls below. **A database's lives (#318).** A batch's name carries the
admission it belongs to: `<dbId>-<admittedAt>-<height>` (dbId 0x-hex
lowercase, `admittedAt` the record's - the Admit's `issuedAt` - and the height,
both decimal; `fleet.store_name`, vectors `fleet.names`). An admission after a
retirement (`Admitted` following `Retired`) starts a new life: its heights
start at 1, the store's view of the database (its latest head, its heights)
is the new life's, a head written under another admission is refused `409`,
and a node rebuilds from the current admission's names only. Every key is
derived from the chain - the record's `admittedAt` - with no state of the
store's own. An old life's names stay readable (verify) until the database's
bucket is deleted; DeleteDb deletes every life's bytes. `<fleet>` is the shared rollup's address, `<db>` a `dbId` (hex).

| Call | By | What |
|------|----|------|
| `POST /fleet/<fleet>/name/<db>-<admittedAt>-<height>`, the body the batch's bytes, the instance (32 bytes) and the `Head` signature (65 bytes); header `X-HPPLite-Head` the head's fields as JSON | the database's sequencer | the batch and its head in one write (#296). Checks: the head's `dbId` and `toHeight` are this name's; its signature recovers to the database's `sequencer`; its `instanceId` and `epoch` are the lease's; `admittedAt` is the record's (another admission's is `409`, as an old epoch's); the bytes' keccak is the batch's. An older epoch is `409`: the fence. Kept: the bytes under their CID, the name `<fleet>-<dbId>-<admittedAt>-<height>`, the admission's latest head |
| `GET /fleet/<fleet>/name/<db>-<admittedAt>-<height>` | - | the bytes, the instance and the signature, of any admission (an old life stays readable for verify) |
| `GET /fleet/<fleet>/heads?after=<dbId>&limit=<n>` | the anchor | every database whose latest head is not the head in its latest indexed root, by `dbId` ascending (the leaf order), paged: what the next root is built from. Defined by state: a head that moved between the anchor's read and the root is listed again by itself |
| `GET /fleet/<fleet>/db/<db>` | - | `{dbId, epoch, sequencer, instance, retired, admittedAt, latestHead, latestRoot}`, `latestHead` the current admission's; `latestRoot` is a hint, the path proves it |
| `POST /fleet/<fleet>/manifest`, the body the manifest; `X-HPPLite-Signature` the fleet's anchor's `Content(address da = <fleet>, uint256 height = <seq>, bytes32 hash = keccak256(body))` in the store's domain | the anchor, before `postRootFor` | the manifest kept under its CID (`GET /get/<cid>`). Free (somaro), so bounded: only the fleet's anchor's signature (`401` otherwise), only for a root not yet posted (`409` for `seq <= rootSeq`, but for a posted root's own manifest put late - its sha-256 the digest the root names - while the store holds the root as "manifest missing"), one per such root (a newer one for the same `seq` replaces it: an anchor whose `postRootFor` failed rebuilds), and one no root on chain names is swept after the store's sweep age, as bytes in no bucket |
| `POST /fleet/<fleet>/root` | anyone | a hint to read the chain now; its body is trusted for nothing |

**The chain's side comes from the fleet's events.** The store reads
`eth_getLogs` for a fleet whenever its view is older than a few seconds (the
reference store's `--fleet-follow`, 5 s) before it checks a head or shows a
database - `Admitted`, `Claimed`,
`EpochMoved`, `SequencerSet`, `Retired`, `Promoted`, `RootPosted` - and keeps
sequencer, instance, epoch and retirement for every database: a cost that
does not grow with databases or writes. A head whose epoch or instance is
ahead of what the store holds makes it read that one database on chain at
once, so a new holder's first write is not refused for the store being behind.
(v10's store reads the rollup's roles at a write, cached and read again on a
mismatch: per write, not a stronger guarantee in kind.)

**The bound.** After a takeover on chain (`forceExpire`, `retireFor`,
`setSequencer`, a new holder's claim), the store takes the old holder's writes
for at most that interval plus a block; a refusal reads the database's record
first, so nothing is refused on a view the chain has left (#317). What the old
holder wrote in that window is history signed by the database's own key, on
the same rolling hash: a new holder that waits for its epoch at the store and
then rebuilds includes it, so nothing is lost; a name, once written, is never
replaced, which is what makes that safe.

**The node fences itself too.** A node holding the lease at an epoch stops
writing for the rest of its run once the record shows another epoch - read at
its lease checks, or at once after the store refuses its head as not the
lease's epoch. It stays up, claims nothing and reads nothing more, and says
`hpplite: fenced: ...` once with the epoch it held and the record's; a restart
is a new run, which may claim like any other.

**The node's part of the fence.** After a claim, the new holder waits until
`GET /fleet/<fleet>/db/<db>` shows its own epoch before it reads names to
rebuild; from then the store refuses the old epoch, so nothing the old holder
writes after the takeover is read.

**Roots come from the same events.** `RootPosted(seq, root, manifestDigest)`
names the manifest; the store takes it from its own content by CID, checks
its sha-256 against the digest and its leaves against the root, and then
records `latestRoot = seq` for each `dbId` in it. The anchor's order is fixed:
the manifest put first, then `postRootFor`. A root whose manifest is not at
the store is kept as "manifest missing" and tried again; it is no
`latestRoot` until it checks.

The reference store serves the calls above (`operators/store/fleet_store.py`,
ctest `test_store_fleet` on anvil). A head's chain hash must follow from the
previous head and the bytes (`keccak256(abi.encode(prev, height,
keccak256(bytes)))`, zero before height 1), which ties the signed head to the
bytes it is written with. A write past a gap, or above height 1 for a database
whose earlier heads the store never held (a migration, a second store), is
`409` with the height it expects; above height 1 the way in is the genesis
fields of `admitFor` (not read by the reference store yet: the migration
step). A store that charges takes a fleet write only into a bound database's
bucket, charged and capped as a rollup's; an unbound database's write gets the
same `402 bucket_unbound` an unbound rollup's does.

**Funding and caps.** One bucket per `(fleet, dbId)`. Its messages, in the
store's domain `("HPPLiteStore", "2", chainId, payTo)`, are today's four over
`(fleet, dbId)` (#310; vectors in `bucketMessages`):

| Type | Signed by |
|------|-----------|
| `BindDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)` | the funder, and the database's side: with a record, its sequencer, or its owner alone (as #273); before its record exists, its owner alone, the request carrying `owner` and `nameHash` so the store checks `dbId = keccak256(abi.encode(owner, nameHash))` - no chain read |
| `ReleaseDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)` | the funder |
| `DeleteDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)` | the funder |
| `CapDb(address fleet,bytes32 dbId,uint256 writesPerDay,uint256 bytesPerDay,uint256 issuedAt)` | the funder |

Calls: `POST /fleet/<fleet>/db/<dbId>/bucket` `{funder, issuedAt,
funderSignature, dbSignature[, owner, nameHash]}` (BindDb; a newer one from
another funder rebinds, as Bind does, and the cap goes with the old funder);
`.../bucket/cap` `{funder, writesPerDay, bytesPerDay, issuedAt, signature}`;
`.../bucket/release` `{funder, issuedAt, signature[, delete: true]}`
(ReleaseDb, or DeleteDb with `delete`). The reference store serves them
(#311).

A released (parked) database's content is not served, as a parked rollup
bucket's: `GET /get/<cid>` of its batches answers `410 bucket_parked` (#323;
data nobody pays for is not served). After DeleteDb its bytes are gone - `GET`
of its names answers 404 - while its view keeps `latestHead`, the record of
what it held. On a free store a fleet database's writes need no bucket; with
none they are not counted in the store's quotas, and they are served.

A bucket bound before the record costs its funder nothing until bytes come,
and bytes come only with a head the fleet's record accepts; the record's owner
is the binding's (#307). The anchor's manifest is signed as `Content` with
the fleet in `da` and the root's sequence as its height (with a vector); it is
free and needs no bucket. The listing: `GET /account/<funder>` names
fleet databases apart, `databases: [{fleet, dbId}]`, `buckets` staying
addresses; one database's bucket is `GET /fleet/<fleet>/db/<dbId>/bucket`,
today's bucket body with `fleet` and `dbId` in place of `da`. The database's
view (`GET /fleet/<fleet>/db/<dbId>`) also carries `anchored: {seq, root,
toHeight, time, path}` - the latest indexed root holding it, that root, its
head's height there, RootPosted's block time, and the leaf's path into the
root (`[{hash, left}]`, siblings bottom up; empty when the root is the leaf).
It is a hint, the path its proof against the root on chain. A root is a delta:
a database absent from a later root keeps the `anchored` of the last root that
held it, never moved back, and a root indexed late never replaces a newer one.

One BindDb per database, the fleet's treasury (or a database's own funder in a
common fleet) as funder, a cap per database; the name write is charged to
that bucket. Admission and payment stay two decisions by two keys. The
anchor's manifests are free (somaro's decision), bounded as the manifest row
says: one per root not yet posted, the anchor's alone, swept when no root
names them.

**Limits.** Every fleet call is counted per `dbId` (the signed head names
it), the per-address guard far above it: a platform's apps write from one
address. The relay counts a fleet's joins and claims per `dbId` and its roots
per fleet.

**Retirement.** A retired database's names stay until its bucket is
released, as a released bucket's do today.

## Snapshots and pruning (#314)

Without them a fleet database's history at the store only grows and a
rebuild replays every batch from height 1. A **snapshot** is the database's
state at a height `S`: one page image (every page, as a rollup's snapshot,
#221), sealed as a batch at `S` is, cut into chunks of at most 8 MiB, and an
index (`fleet.snapshot_index`, canonical JSON):

    {"admittedAt", "at", "block", "bytes", "chain", "chunks": ["0x<sha-256>", ...],
     "dbId", "height", "protocol": "hpplite-fleet/1", "prune", "rollup", "root"}

`root` and `chain` are the head's `stateRoot` and `chainHashAfter` at `S`,
`block` the bookkeeping's block hash there, `bytes` the chunks' size in all,
`prune` whether the history below it may go. Each piece is signed by the
database's sequencer as `Snapshot(dbId, admittedAt, S, part, sha-256)`.

| Call | By | What |
|------|----|------|
| `POST /fleet/<fleet>/snapshot/<db>-<admittedAt>-<S>/<part>`, the body the piece's bytes; `X-HPPLite-Signature` its `Snapshot` signature | the database's sequencer | chunks first (parts 1..n), the index last (part 0). Checks: the signature recovers to the record's sequencer, `admittedAt` is the record's; for the index, its fields are this name's, the store holds the head at `S` of this admission and the index's `root` and `chain` are that head's, and every chunk it names is held. Charged to the database's bucket as a name write. `200 {cid, bytes}` |
| `GET /fleet/<fleet>/db/<db>` | - | also `snapshot: {height, index, signature, at, prune}` - the current admission's latest checked snapshot, `index` the CID of its index - and `prunedBelow` (0: nothing pruned) |

**Rebuilding from a snapshot.** A node whose file is behind the store (an
empty file, a lost volume) and below the latest snapshot `S` - or below
`prunedBelow`, where the batches are gone - starts there: the index by its
CID, its `Snapshot` signature recovering to the database's key; each chunk by
its sha-256; the image opened with the database's keys and its root the
index's `root`; written as the file and the bookkeeping set at `S`
(`block`, `chain`, `root`); then batches `S+1` to the latest head as before,
and the rolling hash after the last must be the latest head's
`chainHashAfter`, signed by the database's key. Nothing is written to the
file before every check up to the image's root has passed.

**Verifying from a snapshot.** The anchored head at `T >= S` signs
`chainHashAfter(T)` and `stateRoot(T)`; the batches `S+1..T` roll from the
snapshot's `chain` to that hash and apply from its image to that root. A
snapshot that is not the history - another image, another chain hash - does
not reach the anchored head. `verify` (the leaf's path into the root on
chain) is unchanged.

**Pruning, the store's rule** (`fleet.prune_height`, vectors
`fleet.snapshot.prune`). The store may delete a database's names below `P`
and the pieces of its snapshots below `P`, where `P` is the highest checked
snapshot of the current admission with `prune` set, at `S >= 2`, at or under
the height of the database's anchored head - and that anchored head is of the
same admission (the head the store holds at that height is the root's). It
prunes on its own when a root is indexed or a snapshot is checked, and never
before the root on chain holds a head at or past `S`. Kept: the names from
`P` on (the head at `P` with them), the snapshot at `P` and any above it.
A bucket's bytes then stay near one snapshot and the batches since the last
anchor. Names once pruned are never written again: a write below
`prunedBelow` is `409`. `GET /fleet/<fleet>/db/<db>/bucket` shows the bytes
after the prune.

**The node's part.** With `fleet=` the node takes a snapshot while it holds
the lease, after a flush, when 100 batches have gone since the last one or
`snapshot=` (a duration, default 24 h) has passed with any batch in between;
`snapshot=0` takes none. `keep=all` keeps the history (`prune: false` in the
index); `keep=<duration>` is not taken with `fleet=`. A snapshot the store did
not take is said once and tried at the next flush; the batches are written
regardless.

## The relay (sponsorship of a fleet's calls; vectors first, #312)

In the relay's domain `("HPPLiteRelay", "1", chainId, payTo)`, today's three
messages over `(fleet, dbId)` (vectors in `relayMessages`):

| Type | Signed by |
|------|-----------|
| `SponsorDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)` | the funder, and the database's side: with a record its owner or its sequencer; before the record (a join is the first call) its owner alone, the request carrying `owner` and `nameHash`, the relay checking the derivation without a chain read |
| `UnsponsorDb(address fleet,bytes32 dbId,address funder,uint256 issuedAt)` | the sponsor |
| `CapDb(address fleet,bytes32 dbId,uint256 callsPerDay,uint256 issuedAt)` | the sponsor: a database's joins and claims a day |

A fleet's roots are sponsored with today's `Sponsor` and `Cap`, the fleet in
place of the rollup and the fleet's owner as the other side (not the anchor):
roots are counted per fleet. A database's sponsor pays its `joinFor`,
`claimFor`, `retireFor` and `expireFor`; CapDb counts joins and claims, and
`retireFor` and `expireFor` are charged but never refused by the cap nor
counted (they are the owner's: a retirement once per record; somaro's order at `rm` is retire, UnsponsorDb, DeleteDb). No fallback
between the two: a database with no SponsorDb is not covered by the fleet's
Sponsor, and the fleet's Sponsor pays `postRootFor` only. `createFleetFor` has
no sponsor (no fleet yet): its caller pays over x402, as for `createRollupFor`.
An unsponsored, capped or unfunded call keeps today's `402` (with `sponsor:
{funder, why}` when there is a sponsor), so the caller may still pay itself. The
relay tells a fleet from a rollup by the v11 factory's own record
(`isFleet`), never by probing; a `to` that is neither is refused as today.

Calls, mirroring the store's: `POST /fleet/<fleet>/db/<dbId>/sponsor`
`{funder, issuedAt, funderSignature, dbSignature[, owner, nameHash]}` (a newer
SponsorDb from another funder re-sponsors, the cap going with the old funder);
`.../sponsor/cap` `{funder, callsPerDay, issuedAt, signature}`;
`.../sponsor/release` `{funder, issuedAt, signature}`; `GET
/fleet/<fleet>/db/<dbId>/sponsor`; `GET /account/<funder>` lists `databases:
[{fleet, dbId}]` apart from `rollups`. The reference relay serves them with
`--fleet-factory` (#313; ctest `test_relay_fleet` on anvil).

## The node (fleet=, #316)

`fleet=<shared rollup>&owner=<address>&dbname=<name>` names the database; the
node derives its dbId and prints it at open. `store=` and `relay=` are
required. The lease is the fleet's record of the database: the node reads
`db(dbId)` and, when the lease is not its own, takes it through the relay -
`joinFor` with the fleet's Admit (`admit=<issuedAt>:0x<signature>`) when the
fleet has no live record, `claimFor` with the record's `claimNonce`
otherwise. A lease that is its own (same instance, same key) with more than
half of the fleet's lease duration left makes no call - not at open, not at a
flush, not at close; in the second half the next post renews it (`claimFor`
by the holding instance, the epoch unchanged). With somaro's 7-day lease a
database costs about one chain call in three and a half days of writing, and
none while idle. There is no release: the lease runs out at close, and an
operator who wants fast failover uses the owner's `forceExpire` or its signed
`expireFor` through the relay, not a short lease. A database the fleet has no live record of, opened without `admit=`,
stops with one line saying the Admit is needed.

Each flush writes its batch to `/fleet/<fleet>/name/<dbId>-<height>` with the
head (`fromHeight = toHeight =` the batch's height, the rolling chain hash
after it, the page root, `manifestDigest` the sha-256 of the bytes, the
lease's epoch) signed by the database's key in `X-HPPLite-Head`; the node
posts no root and no anchor (the fleet's anchor does). A file that already
has rows when first opened (written by the app or a migration before its
sequencer) is a first open: the join, then its state as batch 1. A file
behind the heads the store holds (a fresh or emptied file, a new volume) is
rebuilt from them: the node takes the lease, waits until
`GET /fleet/<fleet>/db/<dbId>` shows its epoch and instance (the fence: from
then the store refuses the old holder), reads every name from the file's
height to the latest head's, applies them (the seal is the database's key),
and checks the rolling hash after the last against the latest head's
`chainHashAfter`, whose signature must recover to its own key; then it writes
on. While another instance holds the lease, such a file is not opened as the
sequencer.

**What the file is at open (somaro decision 0018).** Before any claim:
1. Bookkeeping beside the file, but the file is not what it last sent (the app
   deleted its database and made another at the same path, or wrote with no
   sequencer running): the file goes whole as the next height N+1, whatever
   is set; said once, `hpplite: <file> is not the state of height N it last
   sent: the file goes whole as height N+1`.
   The same check runs while the node holds the lease (hpplite#328), so a
   divergence the WAL never showed is not left to the next start: every
   `HPPLITE_FILE_CHECK_SECONDS` (60) when the file's stat changed, once its
   own checkpoint put the whole WAL in the file with nothing pending; the
   same line, the file whole as the next height.
2. No bookkeeping, a file with pages (a schema, or more than an empty file's
   page), and a history at the store at N > 0: which is true is not known
   here, so `onforeign=` (or `HPPLITE_ON_FOREIGN`; the parameter wins) decides.
   Unset, the open fails before the sidecar or the page capture touch the file
   and before any claim: `<file> has pages and no bookkeeping, and the fleet's
   history of this database is at height N: refused; onforeign=history rebuilds
   the file from the history, onforeign=file sends the file as height N+1`.
   `history`: the lease, the store at this run's epoch, then the file emptied
   and rebuilt - byte for byte what a rebuild of an empty file gives (the
   batches are page diffs from an empty file); `<file> replaced by the fleet's
   history at height N (onforeign=history)`. `file`: the lease, then the
   bookkeeping as of N (batch N's hash, the signed head's rolling hash and
   root) and the file whole as N+1; `<file> sent whole as height N+1 over the
   fleet's history (onforeign=file)`. Another value refuses the open.
3. An empty or absent file with a history rebuilds (above); a file the
   bookkeeping last sent opens; a database with no history and a file with
   pages is a first open (the file as batch 1). `onforeign=` plays no part.

## Not decided here

The contract (`contracts/src/HPPLiteFleet.sol`, `HPPLiteFleetFactory.sol`,
#300-#303, #321) is deployed on HPP Sepolia: the fleet factory
`0x2880d4136720bd5dac2d3b16eb63e4f063ccfea2` (2026-11-11). The reference store
and relay serve fleets (#310-#317), and so do HPP's canonical ones
(hpplite-operators #42-#44) except a fleet's roots; the node's `fleet=` mode is
built (#316). The roots are built: the store's manifests and its indexing of
`RootPosted` (the reference and HPP's canonical store), the builder's rule
with a vector, the reference `hpplite-anchor` and `verify` (somarod anchors
its own fleet).
