#!/bin/sh
# hpplite-store/2's selftest (#213) on a fresh anvil with the factory, MockUSDC
# (permit, #212) and the mock facilitator (tests/l1/anvil_test.sh). ctest test_store_buckets; needs foundry.
here=$(cd "$(dirname "$0")" && pwd)
[ -n "${HPPLITE_ANVIL_RPC:-}" ] || exec sh "$here/../../tests/l1/anvil_test.sh" "$here/bucket_selftest.sh"
exec python3 -I "$here/bucket_selftest.py"
