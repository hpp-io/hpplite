#!/bin/sh
# Fail if a factory address in the docs is not one of the networks' in da_uri.c (HPPLITE_NETWORKS:
# hpp-sepolia, hpp-mainnet), or if the README leaves one of them out (#416).
# Usage: tools/check_docs.sh <ext/hpplite dir>
set -eu
DIR="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
CODE=$(grep -oiE '\{"[a-z0-9-]+",[^}]*"0x[0-9a-f]{40}"' "$DIR/da_uri.c" | grep -oiE '0x[0-9a-f]{40}' | tr 'A-F' 'a-f' | sort -u)
[ -n "$CODE" ] || { echo "check_docs: no factory in da_uri.c"; exit 1; }
echo "$CODE" | grep -q "$(grep -oiE '\{"hpp-sepolia",[^}]*"0x[0-9a-f]{40}"' "$DIR/da_uri.c" | grep -oiE '0x[0-9a-f]{40}' | tr 'A-F' 'a-f')" \
  || { echo "check_docs: no hpp-sepolia factory in da_uri.c"; exit 1; }
rc=0
for f in README.md examples/README.md test_util.h .env.example; do
  for a in $(grep -oiE '0x[0-9a-f]{40}' "$DIR/$f" | tr 'A-F' 'a-f' | sort -u); do
    if echo "$CODE" | grep -qx "$a" || [ "$a" = 0x0000000000000000000000000000000000000000 ]; then continue; fi
    # only flag addresses presented as the factory
    if grep -iE "factory" "$DIR/$f" | grep -qi "$a"; then
      echo "check_docs: $f names factory $a, which is no network's in da_uri.c ($(echo $CODE))"; rc=1
    fi
  done
done
# every network's factory is in the README
for a in $CODE; do
  grep -qi "$a" "$DIR/README.md" || { echo "check_docs: da_uri.c has the factory $a, README.md does not name it"; rc=1; }
done
# the canonical operators' URLs (#192): what store=on / relay=on take must be what the README says they take
for u in $(grep -oE '"https://(store|relay)\.[a-z0-9.]+"' "$DIR/da_uri.c" | tr -d '"'); do
  grep -q "$u" "$DIR/README.md" || { echo "check_docs: da_uri.c has $u, README.md does not name it"; rc=1; }
done
[ $rc -eq 0 ] && echo "check_docs: factory addresses and operator URLs consistent ($(echo $CODE))"
exit $rc
