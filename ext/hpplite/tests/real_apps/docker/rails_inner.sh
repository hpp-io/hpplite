#!/bin/sh
# The inside of tests/real_apps/docker/rails.sh, run by anvil_test.sh with
# HPPLITE_ANVIL_RPC/FACTORY/KEY0 in the environment: the Rails app's session
# in the container (db:prepare, Puma with two workers, Solid Queue's jobs,
# 16 clients x 50 operations), then the four databases copied out and a
# replica of each rebuilt from the chain on the host, found by name.
set -u
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_RAILS_IMAGE:-hpplite-rails}
port=${HPPLITE_ANVIL_RPC##*:}
base=${HPPLITE_APPS_OUT:-$HOME/.cache/hpplite-apps}; mkdir -p "$base"
out=$(cd "$(mktemp -d "$base/rails-XXXXXX")" && pwd -P)
echo "== session in the container (databases copied to $out)"
$runtime run --rm -v hpplite-apps-build:/work:ro -v "$out:/out" --add-host host.docker.internal:host-gateway \
    -e HPPLITE_EXT=/work/build/hpplite.so -e RAILS_ENV=production -e SECRET_KEY_BASE=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef \
    -e HPPLITE_FILES="*/storage/production*.sqlite3" \
    -e HPPLITE_PARAMS="rpc=http://host.docker.internal:$port&factory=$HPPLITE_ANVIL_FACTORY&chainid=31337&privkey=$HPPLITE_ANVIL_KEY0&interval=1s&dbname={file}" \
    "$image" sh -ec '
        bin/rails db:prepare > /tmp/prepare.log 2>&1 || { tail -20 /tmp/prepare.log; exit 1; }
        WEB_CONCURRENCY=2 RAILS_MAX_THREADS=5 bin/rails server -p 3010 > /tmp/server.log 2>&1 &
        srv=$!
        bin/jobs > /tmp/jobs.log 2>&1 &
        jobs=$!
        for i in $(seq 1 90); do curl -s -o /dev/null http://localhost:3010/products.json && break; sleep 1; done
        curl -s -o /dev/null -w "rails up: HTTP %{http_code}\n" http://localhost:3010/products.json
        python3 /suite/drive.py http://localhost:3010 16 50 > /tmp/drive.log 2>&1; rc=$?
        tail -6 /tmp/drive.log; echo "drive exit $rc"
        sleep 8                                     # the jobs finish, the timers post (interval 1s)
        kill -TERM $jobs $srv; wait $jobs $srv 2>/dev/null || true
        mkdir -p /out/db && cp -a storage/. /out/db/ && cp /tmp/*.log /out/
        for f in /tmp/server.log /tmp/jobs.log /tmp/prepare.log; do grep "hpplite:" $f | sort | uniq -c | sort -rn | head -5 | sed "s|^|   $(basename $f): |"; done
        chmod -R a+rX /out
        ls -la /out/db | grep sqlite3 | sed "s/^/   /"
        exit $rc
    '
rc=$?
echo "== replica checks on the host, each database by its name"
py=${HPPLITE_ORM_CACHE:-${TMPDIR:-/tmp}/hpplite-orm}/venv/bin/python; [ -x "$py" ] || py=python3
for db in production.sqlite3 production_cache.sqlite3 production_queue.sqlite3 production_cable.sqlite3; do
    echo "-- $db"
    HPPLITE_EXT=$ext/build_real/hpplite.dylib HPPLITE_ORM_DIR=$out/db HPPLITE_ORM_EXTRA="&dbname=$db" "$py" "$ext/tests/orm/orm_common.py" "$db" || rc=1
done
if [ $rc = 0 ]; then rm -rf "$out"; else echo "kept for a look: $out"; fi
exit $rc
