#!/bin/sh
# The store's side of hpplite-fleet/1 (#297) on a fresh anvil: the v11 factory deployed here, a fleet, a database
# joined, heads with their name writes, the epoch fence, a root and its manifest (tests/l1/anvil_test.sh).
# ctest test_store_fleet; needs foundry.
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${HPPLITE_ANVIL_RPC:-}" ] || exec sh "$here/../../tests/l1/anvil_test.sh" "$here/fleet_selftest.sh"
exec python3 -I "$here/fleet_selftest.py"
