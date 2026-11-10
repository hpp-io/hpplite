#!/bin/sh
# The relay's v11 calls (#313) on a fresh anvil: the fleet factory deployed here, a fleet, a database sponsored and
# joined through the relay, the cap, retirement, the fleet's roots on its own sponsor (tests/l1/anvil_test.sh).
# ctest test_relay_fleet; needs foundry.
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${HPPLITE_ANVIL_RPC:-}" ] || exec sh "$here/../../tests/l1/anvil_test.sh" "$here/fleet_selftest.sh"
exec python3 -I "$here/fleet_selftest.py"
