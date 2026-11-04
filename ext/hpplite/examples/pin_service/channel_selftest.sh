#!/bin/sh
# The pin service's batch-settlement selftest (#135) on a fresh anvil with
# MockUSDC and x402's escrow (tests/l1/anvil_test.sh). ctest test_pin_service_channels; needs foundry.
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${HPPLITE_ANVIL_RPC:-}" ] || exec sh "$here/../../tests/l1/anvil_test.sh" "$here/channel_selftest.sh"
exec python3 -I "$here/channel_selftest.py"
