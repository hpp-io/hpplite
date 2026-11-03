#!/bin/sh
# The inside of tests/real_apps/docker/homeassistant.sh, run by anvil_test.sh
# with HPPLITE_ANVIL_RPC/FACTORY/KEY0 in the environment: Home Assistant's
# recorder on HPPLite for a while (the automation ticks every second, a purge
# and repack every 60 ticks), a SIGTERM, then the database copied out and a
# replica rebuilt from the chain on the host compared with it.
set -u
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_HA_IMAGE:-hpplite-homeassistant}
secs=${HPPLITE_HA_SECONDS:-150}
port=${HPPLITE_ANVIL_RPC##*:}
base=${HPPLITE_APPS_OUT:-$HOME/.cache/hpplite-apps}; mkdir -p "$base"
out=$(cd "$(mktemp -d "$base/ha-XXXXXX")" && pwd -P)
echo "== session in the container, $secs s (database copied to $out)"
$runtime run --rm -v hpplite-apps-build:/work:ro -v "$out:/out" --add-host host.docker.internal:host-gateway \
    -e HPPLITE_EXT=/work/build/hpplite.so -e HPPLITE_FILES="*/home-assistant_v2.db" -e HA_DB_URL="sqlite:////ha/config/home-assistant_v2.db" \
    -e HPPLITE_PARAMS="rpc=http://host.docker.internal:$port&factory=$HPPLITE_ANVIL_FACTORY&chainid=31337&privkey=$HPPLITE_ANVIL_KEY0&datadir=/ha/config&interval=2s" \
    -e SECS="$secs" "$image" sh -ec '
        rm -f /.dockerenv 2>/dev/null || true        # inside Docker, Home Assistant does not install what its integrations need (the official image carries it all); this image does not
        hass -c /ha/config > /tmp/hass.log 2>&1 &
        h=$!
        for i in $(seq 1 600); do curl -s -o /dev/null http://localhost:8124/ && break; sleep 1; done   # a first start installs its integrations: minutes
        echo "home assistant up after ${i}s"
        sleep "$SECS"
        kill -TERM $h; wait $h || true
        mkdir -p /out/db; cp -a /ha/config/home-assistant_v2.db* /out/db/ 2>/dev/null || true; cp /tmp/hass.log /out/
        rows=$(sqlite3 /out/db/home-assistant_v2.db "select count(*) from states" 2>/dev/null || echo 0)
        echo "   ticks: $rows state rows"
        [ "$rows" -gt 0 ] || { echo "-- no states recorded; the log tail:"; grep -vE "^\s*$" /tmp/hass.log | tail -30; }
        grep -c "hpplite:" /tmp/hass.log | sed "s/^/   hpplite messages in the log: /"
        grep "hpplite:" /tmp/hass.log | sort | uniq -c | sort -rn | head -8 | sed "s/^/   /"
        grep -iE "error|refused" /tmp/hass.log | grep -i hpplite | head -5 | sed "s/^/   /"
        chmod -R a+rX /out
        ls -la /out/db | sed "s/^/   /"
        [ "$rows" -gt 0 ]
    '
rc=$?
echo "== replica check on the host"
py=${HPPLITE_ORM_CACHE:-${TMPDIR:-/tmp}/hpplite-orm}/venv/bin/python; [ -x "$py" ] || py=python3
HPPLITE_EXT=$ext/build_real/hpplite.dylib HPPLITE_ORM_DIR=$out/db "$py" "$ext/tests/orm/orm_common.py" home-assistant_v2.db || rc=1
if [ $rc = 0 ]; then rm -rf "$out"; else echo "kept for a look: $out"; fi
exit $rc
