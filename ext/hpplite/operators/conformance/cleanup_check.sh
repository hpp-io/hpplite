#!/bin/sh
# selftest.sh leaves nothing running (#196): run it under a fresh TMPDIR, then no process may still name that
# directory (its stores were started with --dir inside it). ctest test_conformance_cleanup.
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-cleanup.XXXXXX")
TMPDIR="$tmp" sh "$here/selftest.sh" > "$tmp.out" 2>&1
rc=$?
sleep 1
left=$(ps -axo pid=,command= | grep -F "$tmp/" | grep -v grep)
rm -rf "$tmp" "$tmp.out"
if [ -n "$left" ]; then
    echo "FAIL: selftest.sh left $(echo "$left" | wc -l | tr -d ' ') process(es) running:"
    echo "$left" | cut -c1-160
    echo "$left" | awk '{print $1}' | xargs kill 2>/dev/null
    exit 1
fi
[ $rc -eq 0 ] || { echo "FAIL: selftest.sh itself failed ($rc)"; exit 1; }
echo "PASS: selftest.sh passed and left nothing running"
