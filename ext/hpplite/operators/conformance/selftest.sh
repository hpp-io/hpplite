#!/bin/sh
# The conformance suite against the reference store (#189): free, and paid
# (its 402s checked against /status; no facilitator is asked), and against a
# service that claims another protocol, which must not conform. The relay is
# checked by operators/relay/selftest.py on anvil. ctest test_conformance.
set -u
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-conf.XXXXXX")
# the stores' pids in a file: start runs in $(...)'s subshell, where a variable would not reach the trap (#196)
trap 'for p in $(cat "$work/pids" 2>/dev/null); do kill $p 2>/dev/null; done; sleep 0.2; rm -rf "$work"' EXIT INT TERM
port() { python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])'; }
echo 0x2020202020202020202020202020202020202020202020202020202020202020 > "$work/signer.key"
start() {   # name, then the service's extra arguments
    name=$1; shift
    p=$(port)
    python3 -I "$here/../store/pin_service.py" --port "$p" --dir "$work/$name" --signer-key "$work/signer.key" \
        --pay-to 0x1111111111111111111111111111111111111111 "$@" > "$work/$name.log" 2>&1 &
    echo $! >> "$work/pids"
    for i in $(seq 1 50); do curl -s -m 2 "http://127.0.0.1:$p/status" 2>/dev/null | grep -q signer && break; sleep 0.2; done
    echo "http://127.0.0.1:$p"
}
free=$(start free)
paid=$(start paid --facilitator http://127.0.0.1:1)
other=$(start other --advertise-protocol hpplite-store/2)
python3 -I "$here/conformance.py" --store "$free" --store "$paid"
rc=$?
if python3 -I "$here/conformance.py" --store "$other" > "$work/other.out" 2>&1; then
    echo "  FAIL    a store claiming hpplite-store/2 conformed"; rc=1
else
    echo "  ok      a store claiming hpplite-store/2 does not conform: $(grep FAIL "$work/other.out" | head -1 | sed 's/^ *FAIL *//')"
fi
[ $rc -eq 0 ] || for f in "$work"/*.log; do echo "--- $f"; tail -20 "$f"; done
exit $rc
