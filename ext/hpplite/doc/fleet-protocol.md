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
  `admitter` and its `anchor` key, and the roots by sequence.
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
| `Head(bytes32 dbId,bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 chainHashAfter,bytes32 stateRoot,bytes32 manifestDigest,uint256 epoch)` | the database's sequencer, at each flush: today's `Range` with the database and its lease epoch |
| `Admit(bytes32 dbId,address owner,address sequencer,uint256 issuedAt)` | the fleet's owner or its admitter: the first claim of a `dbId` in the fleet needs it, so nobody rides a fleet's roots and sponsorship uninvited |
| `Retire(bytes32 dbId,uint256 issuedAt)` | the database's owner: the record goes, at `rm` |
| `Root(uint256 seq,bytes32 root,bytes32 manifestDigest,uint256 deadline)` | the fleet's anchor key (the owner may change it); sponsored at the relay |

`dbId = keccak256(abi.encode(owner, keccak256(name)))`: a database is its
owner and its name in the fleet; `dbname=` computes it, no scan.

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

## Not decided here

The contract's storage layout and gas, the store's endpoints for heads and
the index, the name format under a shared rollup
(`<shared da>-<dbId>-<height>` is the proposal), and the anchor's schedule are
the next steps' to fix: contract v11 on anvil, the reference store, the node
(`fleet=`), the anchor tool, then verify.
