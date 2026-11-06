#!/bin/sh
# The services' code runs where a Cloudflare Worker would (#181, #183): no
# subprocess (no `cast`), no settle key - x402 settles through a facilitator
# URL; the transactions a service sends itself (the relay's calls, the
# store's permit pulls, #213) are raw transactions over the RPC. The anvil stand-ins (facilitator_mock.py, kubo_mock.py,
# s3_mock.py) and the selftests are test infrastructure and are not checked.
#   portable_check.sh [operators dir]     ctest test_operators_portable
dir=${1:-$(cd "$(dirname "$0")/.." && pwd)}
rc=0
for f in "$dir"/x402seller/*.py "$dir"/store/hpplite_store.py "$dir"/relay/relay.py; do
    hits=$(grep -nE '^\s*(import subprocess|from subprocess)|subprocess\.|settle[-_]key|"cast"' "$f")
    if [ -n "$hits" ]; then
        echo "${f#$dir/}: not portable:"; echo "$hits" | sed 's/^/  /'; rc=1
    fi
done
[ $rc -eq 0 ] && echo "portable_check: the services start no process and hold no settle key"
exit $rc
