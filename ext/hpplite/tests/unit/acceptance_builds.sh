#!/bin/sh
# tools/acceptance.sh builds the real-L1 runs with the examples CI compiles (#261): a real build found with
# HPPLITE_BUILD_EXAMPLES off is reconfigured with them on before it is built, one with them on is left alone,
# and a new one is configured with them. cmake and ctest are stubs here: what is checked is the call, not a build.
#   tests/unit/acceptance_builds.sh <path/to/tools/acceptance.sh>
set -u
script=$1
d=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-acceptance-builds.XXXXXX")
trap 'rm -rf "$d"' EXIT
fail() { echo "acceptance_builds: FAIL: $1"; cat "$d/calls" 2>/dev/null; exit 1; }
mkdir -p "$d/bin" "$d/real" "$d/out"
printf '#!/bin/sh\necho "cmake $*" >>"%s/calls"\n' "$d" >"$d/bin/cmake"
printf '#!/bin/sh\necho "100%% tests passed, 0 tests failed out of 1"\n' >"$d/bin/ctest"
chmod +x "$d/bin/cmake" "$d/bin/ctest"
export PATH="$d/bin:$PATH" HPPLITE_ACCEPTANCE_DIR="$d/out" HPPLITE_BUILD_REAL="$d/real"

echo "HPPLITE_BUILD_EXAMPLES:BOOL=OFF" >"$d/real/CMakeCache.txt"
sh "$script" services >"$d/run" 2>&1   # exit 1: the other three runs have no result; the services result is what counts
grep -q "^passed 1 of 1" "$d/out/services.result" || fail "the services run on stubs did not pass: $(cat "$d/run")"
grep -q -- "-B $d/real .*-DHPPLITE_BUILD_EXAMPLES=ON" "$d/calls" || fail "a real build with the examples off is not reconfigured with them on"
[ "$(grep -n -- "-DHPPLITE_BUILD_EXAMPLES=ON" "$d/calls" | cut -d: -f1)" -lt "$(grep -n -- "--build $d/real" "$d/calls" | cut -d: -f1)" ] \
    || fail "the examples are turned on after the build, not before"

rm "$d/calls"
echo "HPPLITE_BUILD_EXAMPLES:BOOL=ON" >"$d/real/CMakeCache.txt"
sh "$script" anvil >"$d/run" 2>&1
grep -q "^passed 1 of 1" "$d/out/anvil.result" || fail "the anvil run on stubs did not pass: $(cat "$d/run")"
if grep -q -- "-DHPPLITE_BUILD_EXAMPLES" "$d/calls"; then fail "a real build with the examples on was reconfigured"; fi

rm -rf "$d/real" "$d/calls"
sh "$script" anvil >"$d/run" 2>&1 || true   # the stub configures nothing, so ctest has no directory: only the call is checked
grep -q -- "-B $d/real .*-DHPPLITE_USE_REAL_L1=ON.*-DHPPLITE_BUILD_EXAMPLES=ON" "$d/calls" || fail "a new real build is not configured with the examples on"
echo "acceptance_builds: the real-L1 runs build the examples: a build with them off is reconfigured first, one with them on is left, a new one gets them"
