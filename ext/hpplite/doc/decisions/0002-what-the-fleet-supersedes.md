# 0002. What the fleet supersedes, and what HPPLite keeps

2026-11-12. The user's decision, through somaro (its decision 0022 simplifies somaro the same day).
Nothing is deleted before somaro's mainnet beta runs.

## Context

The fleet (hpplite-fleet/1: a shared rollup per platform, a store for the history, store snapshots) is what
somaro runs. HPPLite started as one wallet, one rollup, and keeps paths from then that the fleet replaced
for somaro but that a user of HPPLite on its own still meets.

## Decision

HPPLite keeps its own functionality for a user without a fleet:

- **The per-wallet rollup model**: HPPLiteFactory, HPPLite and HPPLiteDA; `contract=`, `factory=`,
  `checkpoint=` and `anchor=` for rollups; the owner binding; the rollup's leases; and the relay's and the
  store's rollup calls and buckets.
- **`role=verify` and `hpplite-verify`**: a replica rebuilt and checked against the chain.
- **The `l1=file://` and `l1=local` backends**, as test backends.

After the mainnet beta runs, three paths the fleet superseded inside HPPLite are deleted, **one slice each**:

1. **On-chain snapshots and pruning for rollups**: `keep=<duration>` and snapshots named in a rollup's
   manifest (#203, #221-#228). A standalone rollup gets the store's snapshots instead (#314's), which are
   fleet-only today, so this slice first extends them to rollups.
2. **Batches on chain**: the DA contract's batch bytes, `storefallback`, and a rebuild from chain logs. A
   rollup has a store; the chain holds commitments only.
3. **The node paying the store per write through x402** without a funder. A standalone rollup pays by
   bucket permit (a funding wallet or `funder=`), as the fleet does.

Each slice must have green acceptance, and each must show that a standalone rollup still works end to end:
open, write, anchor, rebuild, verify, on the store's snapshots and bucket permits in place of what was
removed.

## Consequences

`l1_eth.c`, `node.c`, the DA contract and the anvil scenarios lose the superseded halves of their code. The
README's parameter table drops `keep=<duration>`, `storefallback` and the per-write payment. Until the beta
runs, the work is #337, #338 and the unreleased mainnet config.
