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
The user, the same day: the main line does not have to contain the fleet.

Decision 0002 is reversed: the per-wallet rollup is HPPLite's model, and none of the three paths 0002 would
have deleted after the beta is deleted. The store's snapshots reach standalone rollups in #351, for
somaro's paid tier.

## Consequences

Removed from the main line on 2026-11-13, once somaro said it needs none of it (its #152 closed): the node
(#352; a `fleet=` left in a URI refuses the open), the reference store and relay and the vectors' fleet
messages (#353), the contracts, `operators/fleet/` and the protocol documents (#354), and the canonical
store and relay Workers (hpp-io/hpplite-operators#53). doc/fleet-protocol.md and doc/shared-rollup.md are on
the branch. The deployed VERSION 11 fleet factory, `0x2880d4136720bd5dac2d3b16eb63e4f063ccfea2` on HPP
Sepolia, stays on chain; nothing new is deployed for it. The store's snapshots (#314) went with the fleet:
#351 builds snapshots for a rollup of its own, from the branch where that helps.
