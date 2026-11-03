#!/bin/sh
# The Linux hpplite.so for the containerized application suites: built on
# Debian trixie (Dockerfile.build), the base of the runtime images, into the
# named volume hpplite-apps-build, which every app container mounts
# read-only at /work.
#   tests/real_apps/docker/so.sh          -> /work/build/hpplite.so in the volume
set -eu
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../../../.." && pwd)
image=${HPPLITE_APPS_BUILD_IMAGE:-hpplite-apps-build}
runtime=${HPPLITE_CONTAINER:-docker}
$runtime build -q -t "$image" -f "$here/Dockerfile.build" "$here" >/dev/null
exec $runtime run --rm -v "$root:/src:ro" -v hpplite-apps-build:/work "$image" sh -ec '
    if [ ! -f /work/sqlite/sqlite3.c ] || [ /src/VERSION -nt /work/sqlite/sqlite3.c ]; then
        mkdir -p /work/sqlite && cd /work/sqlite
        /src/configure >/dev/null && make -s sqlite3.c sqlite3ext.h >/dev/null
    fi
    mkdir -p /work/build && cd /work/build
    [ -f CMakeCache.txt ] || cmake -DHPPLITE_SQLITE_SRC=/work/sqlite -DCMAKE_BUILD_TYPE=Release /src/ext/hpplite >/dev/null
    cmake --build . -j"$(nproc)" -t hpplite_ext 2>&1 | grep -E "warning|error|Error" || true
    ls -l /work/build/hpplite.so
'
