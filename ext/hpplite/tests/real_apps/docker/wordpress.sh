#!/bin/sh
# WordPress on HPPLite, end to end, with the application in a container:
# anvil and the factory on the host (tests/l1/anvil_test.sh), PHP 8.4 and
# WordPress in the image (Dockerfile.wordpress), the Linux hpplite.so from
# the build volume (so.sh), the session (wordpress/session.sh) run inside,
# then the database copied out and a replica rebuilt from the chain on the
# host compared with it (tests/orm/orm_common.py).
#   tests/real_apps/docker/wordpress.sh            needs docker, foundry, build_real/hpplite.dylib
# Exit 0 when the session had no failure and the replica equals the site.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
runtime=${HPPLITE_CONTAINER:-docker}
image=${HPPLITE_WP_IMAGE:-hpplite-wordpress}
[ -f "$ext/build_real/hpplite.dylib" ] || { echo "build_real/hpplite.dylib missing: the replica check needs it"; exit 2; }
echo "== the Linux module"; sh "$here/so.sh"
echo "== the image"; $runtime build -q -t "$image" -f "$here/Dockerfile.wordpress" "$here/.." >/dev/null
# the containers reach anvil through the host gateway: anvil must listen beyond 127.0.0.1
HPPLITE_ANVIL_ARGS="--host 0.0.0.0 ${HPPLITE_ANVIL_ARGS:-}" exec sh "$ext/tests/l1/anvil_test.sh" "$here/wordpress_inner.sh"
