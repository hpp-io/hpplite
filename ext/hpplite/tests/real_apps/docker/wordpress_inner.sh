#!/bin/sh
# The inside of tests/real_apps/docker/wordpress.sh, run by anvil_test.sh with
# HPPLITE_ANVIL_RPC/FACTORY/KEY0 in the environment: the session in the
# container, the replica check on the host.
set -u
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_WP_IMAGE:-hpplite-wordpress}
port=${HPPLITE_ANVIL_RPC##*:}
base=${HPPLITE_APPS_OUT:-$HOME/.cache/hpplite-apps}; mkdir -p "$base"
out=$(cd "$(mktemp -d "$base/wp-XXXXXX")" && pwd -P)      # under the home directory: Docker Desktop shares it with containers (a macOS temp dir came back empty)
echo "== session in the container (database copied to $out)"
$runtime run --rm -v hpplite-apps-build:/work:ro -v "$out:/out" --add-host host.docker.internal:host-gateway \
    -e HPPLITE_PARAMS="rpc=http://host.docker.internal:$port&factory=$HPPLITE_ANVIL_FACTORY&chainid=31337&privkey=$HPPLITE_ANVIL_KEY0" \
    "$image" sh -ec '
        sh /suite/session.sh php /work/build/hpplite.so /wp 8090
        cp /wp/session.log /out/
        cp -a /wp/wordpress/wp-content/database/. /out/db/ 2>/dev/null || { mkdir -p /out/db && cp -a /wp/wordpress/wp-content/database/. /out/db/; }
        chmod -R a+rX /out
        ls -la /out/db | sed "s/^/   /"
    '
fails=$(grep -c "FAIL" "$out/session.log" || true)
echo "== session: $fails failure(s); HPPLite messages:"; grep "hpplite:" "$out/session.log" | sort | uniq -c | sort -rn | head -10 | sed 's/^/   /'
echo "== replica check on the host"
py=${HPPLITE_ORM_CACHE:-${TMPDIR:-/tmp}/hpplite-orm}/venv/bin/python; [ -x "$py" ] || py=python3
HPPLITE_EXT=$ext/build_real/hpplite.dylib HPPLITE_ORM_DIR=$out/db "$py" "$ext/tests/orm/orm_common.py" .ht.sqlite
rc=$?
[ "$fails" = 0 ] || rc=1
if [ $rc = 0 ]; then rm -rf "$out"; else echo "kept for a look: $out"; fi
exit $rc
