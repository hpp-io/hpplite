#!/bin/sh
# The inside of tests/real_apps/docker/ghost.sh, run by anvil_test.sh with
# HPPLITE_ANVIL_RPC/FACTORY/KEY0 in the environment: Ghost's session in the
# container, the replica check on the host.
set -u
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_GHOST_IMAGE:-hpplite-ghost}
port=${HPPLITE_ANVIL_RPC##*:}
base=${HPPLITE_APPS_OUT:-$HOME/.cache/hpplite-apps}; mkdir -p "$base"
out=$(cd "$(mktemp -d "$base/ghost-XXXXXX")" && pwd -P)
echo "== session in the container (database copied to $out)"
$runtime run --rm -v hpplite-apps-build:/work:ro -v "$out:/out" --add-host host.docker.internal:host-gateway \
    -e HPPLITE_PARAMS="rpc=http://host.docker.internal:$port&factory=$HPPLITE_ANVIL_FACTORY&chainid=31337&privkey=$HPPLITE_ANVIL_KEY0" \
    "$image" sh -ec '
        # better-sqlite3 against the stock amalgamation the module was built with (setup.sh does the same on a host)
        b=$(cd /ghost/versions/6.73.0/node_modules/.pnpm/better-sqlite3@*/node_modules/better-sqlite3 && pwd -P)
        gyp=$(dirname "$(dirname "$(command -v npm)")")/lib/node_modules/npm/node_modules/node-gyp/bin/node-gyp.js
        (cd "$b" && node "$gyp" rebuild --release --sqlite3=/work/sqlite >/tmp/gyp.log 2>&1) || { tail -20 /tmp/gyp.log; exit 1; }
        sh /suite/run.sh /ghost /work/build/hpplite.so > /tmp/ghost.log 2>&1 &
        g=$!
        for i in $(seq 1 300); do [ "$(curl -s -o /dev/null -w "%{http_code}" http://localhost:2368/ghost/api/admin/site/)" = 200 ] && break; sleep 1; done
        echo "ghost after ${i}s: HTTP $(curl -s -o /dev/null -w "%{http_code}" http://localhost:2368/ghost/api/admin/site/)"
        rc=0; python3 /suite/drive.py > /tmp/drive.log 2>&1 || rc=$?
        tail -3 /tmp/drive.log; echo "drive exit $rc"
        [ $rc = 0 ] || { echo "-- ghost log tail:"; tail -25 /tmp/ghost.log; }
        sleep 6                                     # the timer posts what the session left (interval 1s); Ghost does not close its database
        kill -INT $g; wait $g || true
        mkdir -p /out/db; cp -a /ghost/content/data/. /out/db/ 2>/dev/null || true; cp /tmp/ghost.log /tmp/drive.log /out/ 2>/dev/null || true
        grep -c "hpplite:" /tmp/ghost.log | sed "s/^/   hpplite messages in Ghost log: /"
        grep "hpplite:" /tmp/ghost.log | sort | uniq -c | sort -rn | head -8 | sed "s/^/   /"
        chmod -R a+rX /out
        ls -la /out/db | sed "s/^/   /"
        exit $rc
    '
rc=$?
echo "== replica check on the host"
py=${HPPLITE_ORM_CACHE:-${TMPDIR:-/tmp}/hpplite-orm}/venv/bin/python; [ -x "$py" ] || py=python3
HPPLITE_EXT=$ext/build_real/hpplite.dylib HPPLITE_ORM_DIR=$out/db "$py" "$ext/tests/orm/orm_common.py" ghost-local.db || rc=1
if [ $rc = 0 ]; then rm -rf "$out"; else echo "kept for a look: $out"; fi
exit $rc
