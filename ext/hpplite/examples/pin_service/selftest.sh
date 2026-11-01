#!/bin/sh
# The pin service in free mode and its buyer's selftest (#125): storage,
# CIDs, receipts and idempotence without a chain. ctest test_pin_service.
#   selftest.sh [port]
set -u
here=$(cd "$(dirname "$0")" && pwd)
port=${1:-$((8440 + $$ % 200))}
work=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-pin.XXXXXX")
echo 0x2020202020202020202020202020202020202020202020202020202020202020 > "$work/signer.key"
python3 -I "$here/pin_service.py" --port "$port" --dir "$work/store" --signer-key "$work/signer.key" \
    --pay-to 0x1111111111111111111111111111111111111111 > "$work/service.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -rf "$work"' EXIT INT TERM
for i in $(seq 1 50); do
    curl -s -m 2 "http://127.0.0.1:$port/status" 2>/dev/null | grep -q signer && break
    sleep 0.2
done
python3 -I "$here/buyer.py" --service "http://127.0.0.1:$port" \
    --key 0x0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20 --selftest
rc=$?
[ $rc -eq 0 ] || { echo "--- service log"; cat "$work/service.log"; }
exit $rc
