#!/bin/sh
# The static build (#269): tools/linux/static/build.sh leaves hpplite-sequencer and hpplite-verify with no
# dynamic dependencies (the Dockerfile refuses a binary ldd can read), and the one binary runs in an Alpine
# and a Debian image alike: in each, on l1=file://, it prints its 'synced height' line (#264). Exit 77 when
# docker (or podman) is missing.
#   tests/unit/static_build.sh <path/to/tools/linux/static/build.sh>
set -u
script=$1
docker=$(command -v docker || command -v podman) || { echo "static_build: docker or podman not found; skipped"; exit 77; }
# under the working directory, not the system temp: the container runtime (colima, Docker Desktop) shares the home
# directory with its VM, and bind mounts from elsewhere arrive empty
d=$(mktemp -d "$(pwd)/hpplite-static-test.XXXXXX")
trap 'rm -rf "$d"' EXIT
fail() { echo "static_build: FAIL: $1"; exit 1; }
sh "$script" "$d/out" >"$d/build.log" 2>&1 || { tail -40 "$d/build.log"; fail "the build did not produce the binaries"; }
[ -x "$d/out/hpplite-sequencer" ] && [ -x "$d/out/hpplite-verify" ] || fail "binaries missing in $d/out"
for image in alpine:3.20 debian:bookworm-slim; do
    "$docker" pull -q "$image" >/dev/null 2>&1 || fail "cannot pull $image"
    mkdir -p "$d/w"
    rm -rf "$d/w/"*
    # a key, so the open takes the lease and the line comes; interval=0: nothing else happens
    out=$("$docker" run --rm -v "$d/out:/b:ro" -v "$d/w:/w" -e "HPPLITE_PARAMS=l1=file:///w/chain&interval=0&datadir=/w&privkey=0x0101010101010101010101010101010101010101010101010101010101010101" \
          "$image" sh -c 'timeout 20 /b/hpplite-sequencer /w/app.db & pid=$!; for i in $(seq 1 100); do kill -0 $pid 2>/dev/null || break; sleep 0.1; done; kill -TERM $pid 2>/dev/null; wait $pid' 2>&1)
    echo "$out" | grep -q "synced height" || { echo "$out" | head -5; fail "the static sequencer did not come up in $image"; }
    echo "static_build: $image: ok"
done
echo "static_build: ok"
