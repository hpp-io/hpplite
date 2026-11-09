#!/bin/sh
# Static hpplite-sequencer and hpplite-verify, built in an Alpine container (#269): one binary for any Linux
# image, no libraries to install. Needs docker (or podman) and the SQLite amalgamation (sqlite3.c, sqlite3.h,
# sqlite3ext.h: the repo's build/ by default, or HPPLITE_SQLITE_SRC).
#   tools/linux/static/build.sh [out-dir] [platform]
# out-dir defaults to ext/hpplite/build_static; platform (linux/amd64, linux/arm64) to the docker host's.
# Exit 77: no docker. The binaries land in out-dir.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
ext=$(cd "$here/../../.." && pwd)
repo=$(cd "$ext/../.." && pwd)
out=${1:-$ext/build_static}
platform=${2:-}
sqlite=${HPPLITE_SQLITE_SRC:-$repo/build}
docker=$(command -v docker || command -v podman) || { echo "static build: docker or podman not found; skipped"; exit 77; }
[ -f "$sqlite/sqlite3.c" ] || { echo "static build: no sqlite3.c in $sqlite (set HPPLITE_SQLITE_SRC, or cd build && ../configure && make sqlite3.c)"; exit 1; }
ctx=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-static.XXXXXX")
trap 'rm -rf "$ctx"' EXIT INT TERM
mkdir -p "$ctx/ext/hpplite" "$ctx/sqlite"
# the sources, not the build directories nor forge's output
(cd "$ext" && tar cf - --exclude='./build*' --exclude='./contracts/out' --exclude='./contracts/cache' --exclude='./contracts/lib' --exclude='./contracts/broadcast' .) | (cd "$ctx/ext/hpplite" && tar xf -)
cp "$sqlite/sqlite3.c" "$sqlite/sqlite3.h" "$sqlite/sqlite3ext.h" "$ctx/sqlite/"
cp "$here/Dockerfile" "$ctx/Dockerfile"
mkdir -p "$out"
# shellcheck disable=SC2086
"$docker" build ${platform:+--platform $platform} -f "$ctx/Dockerfile" -o "type=local,dest=$out" "$ctx"
ls -l "$out/hpplite-sequencer" "$out/hpplite-verify"
