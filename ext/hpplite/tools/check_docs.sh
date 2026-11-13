#!/bin/sh
# Fail if the factory address in the docs differs from the hpp-sepolia alias in da_uri.c.
# Usage: tools/check_docs.sh <ext/hpplite dir>
set -eu
DIR="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
CODE=$(grep -oiE '\{"hpp-sepolia",[^}]*"0x[0-9a-f]{40}"' "$DIR/da_uri.c" | grep -oiE '0x[0-9a-f]{40}' | tr 'A-F' 'a-f')
[ -n "$CODE" ] || { echo "check_docs: no hpp-sepolia factory in da_uri.c"; exit 1; }
rc=0
for f in README.md examples/README.md test_util.h .env.example; do
  for a in $(grep -oiE '0x[0-9a-f]{40}' "$DIR/$f" | tr 'A-F' 'a-f' | sort -u); do
    case "$a" in
      "$CODE"|0x0000000000000000000000000000000000000000) ;;
      *) # only flag addresses presented as the factory
         if grep -iE "factory" "$DIR/$f" | grep -qi "$a"; then
           echo "check_docs: $f names factory $a but da_uri.c has $CODE"; rc=1
         fi ;;
    esac
  done
done
# the canonical operators' URLs (#192): what store=on / relay=on take must be what the README says they take
for u in $(grep -oE '"https://(store|relay)\.[a-z0-9.]+"' "$DIR/da_uri.c" | tr -d '"'); do
  grep -q "$u" "$DIR/README.md" || { echo "check_docs: da_uri.c has $u, README.md does not name it"; rc=1; }
done
[ $rc -eq 0 ] && echo "check_docs: factory address and operator URLs consistent ($CODE)"
exit $rc
