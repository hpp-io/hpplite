# 0003. Fleet mode set aside; 0002 reversed

2026-11-13. The user's decision, after somaro's decision 0026 drops fleet mode.

## Context

The fleet (hpplite-fleet/1, doc/fleet-protocol.md) was built for somaro alone. somaro now backs every app's
volume up to the store through a key's bucket (#349), gives free apps no capture, and plans one standalone
rollup per paid app (owner the customer, sequencer somaro, funder the treasury).

## Decision

Fleet mode is set aside as an experiment found not useful. Its history is kept on the branch
`fleet-experiment` in hpp-io/hpplite (at 8d74e9e0eb) and in hpp-io/hpplite-operators (at 4b77501). Until
somaro's #152 takes it off somaro's staging it stays here as it is, gets no new work, and is not offered to
new users; after that it is removed from the main line, one slice per issue, each with green acceptance.

Decision 0002 is reversed: the per-wallet rollup is HPPLite's model, and none of the three paths 0002 would
have deleted after the beta is deleted. The store's snapshots reach standalone rollups in #351, for
somaro's paid tier.

## Consequences

README, doc/fleet-protocol.md and doc/shared-rollup.md say fleet mode is experimental and set aside.
The deployed VERSION 11 fleet factory stays on chain; nothing new is deployed for it.
