#!/bin/sh
# The relay's selftest (#170) on a fresh anvil with the factory and MockUSDC
# (tests/l1/anvil_test.sh). ctest test_relay; needs foundry.
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${HPPLITE_ANVIL_RPC:-}" ] || exec sh "$here/../../tests/l1/anvil_test.sh" "$here/selftest.sh"
exec python3 -I "$here/selftest.py"
