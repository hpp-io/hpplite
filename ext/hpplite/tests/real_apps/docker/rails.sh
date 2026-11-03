#!/bin/sh
# Rails 8 on HPPLite, end to end, with the application in a container: anvil
# and the factory on the host (tests/l1/anvil_test.sh), Ruby 3.3 and the app
# in the image (Dockerfile.rails), the Linux hpplite.so from the build volume
# (so.sh), the production session (rails/drive.py) run inside, then the four
# databases copied out and a replica of each rebuilt from the chain on the
# host, found by its name (dbname={file}).
#   tests/real_apps/docker/rails.sh            needs docker, foundry, build_real/hpplite.dylib
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_RAILS_IMAGE:-hpplite-rails}
[ -f "$ext/build_real/hpplite.dylib" ] || { echo "build_real/hpplite.dylib missing: the replica check needs it"; exit 2; }
echo "== the Linux module"; sh "$here/so.sh"
echo "== the image"; $runtime build -q -t "$image" -f "$here/Dockerfile.rails" "$here/.." >/dev/null
HPPLITE_ANVIL_ARGS="--host 0.0.0.0 ${HPPLITE_ANVIL_ARGS:-}" exec sh "$ext/tests/l1/anvil_test.sh" "$here/rails_inner.sh"
