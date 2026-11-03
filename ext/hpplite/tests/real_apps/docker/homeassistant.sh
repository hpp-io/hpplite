#!/bin/sh
# Home Assistant on HPPLite, end to end, with the application in a container:
# anvil and the factory on the host (tests/l1/anvil_test.sh), Python 3.13 and
# Home Assistant in the image (Dockerfile.homeassistant), the Linux
# hpplite.so from the build volume (so.sh), the recorder running under the
# suite's configuration for a while inside, then the database copied out and
# a replica rebuilt from the chain on the host compared with it.
#   tests/real_apps/docker/homeassistant.sh     needs docker, foundry, build_real/hpplite.dylib
#   HPPLITE_HA_SECONDS: how long the recorder runs (default 150: two purge-and-repack cycles)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_HA_IMAGE:-hpplite-homeassistant}
[ -f "$ext/build_real/hpplite.dylib" ] || { echo "build_real/hpplite.dylib missing: the replica check needs it"; exit 2; }
echo "== the Linux module"; sh "$here/so.sh"
echo "== the image"; $runtime build -q -t "$image" -f "$here/Dockerfile.homeassistant" "$here/.." >/dev/null
HPPLITE_ANVIL_ARGS="--host 0.0.0.0 ${HPPLITE_ANVIL_ARGS:-}" exec sh "$ext/tests/l1/anvil_test.sh" "$here/homeassistant_inner.sh"
