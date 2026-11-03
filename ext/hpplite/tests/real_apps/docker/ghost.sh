#!/bin/sh
# Ghost on HPPLite, end to end, with the application in a container: anvil
# and the factory on the host (tests/l1/anvil_test.sh), Node 24 and Ghost in
# the image (Dockerfile.ghost), the Linux hpplite.so and the SQLite
# amalgamation from the build volume (so.sh), Ghost's Admin API session
# (ghost/drive.py) run inside, then the database copied out and a replica
# rebuilt from the chain on the host compared with it.
#   tests/real_apps/docker/ghost.sh            needs docker, foundry, build_real/hpplite.dylib
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_GHOST_IMAGE:-hpplite-ghost}
[ -f "$ext/build_real/hpplite.dylib" ] || { echo "build_real/hpplite.dylib missing: the replica check needs it"; exit 2; }
echo "== the Linux module"; sh "$here/so.sh"
echo "== the image"; $runtime build -q -t "$image" -f "$here/Dockerfile.ghost" "$here/.." >/dev/null
HPPLITE_ANVIL_ARGS="--host 0.0.0.0 ${HPPLITE_ANVIL_ARGS:-}" exec sh "$ext/tests/l1/anvil_test.sh" "$here/ghost_inner.sh"
