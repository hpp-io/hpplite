#!/bin/sh
# tools/acceptance.sh --report refuses to report a pass while any of the four runs did not happen or did not
# pass (#258): with results for three runs it exits 1 naming the fourth; with a failed one it exits 1 naming it;
# with four passes it exits 0 and prints one line per run with what it proves.
#   tests/unit/acceptance_report.sh <path/to/tools/acceptance.sh>
set -u
script=$1
d=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-acceptance.XXXXXX")
trap 'rm -rf "$d"' EXIT
export HPPLITE_ACCEPTANCE_DIR=$d
fail() { echo "acceptance_report: FAIL: $1"; cat "$d/out" 2>/dev/null; exit 1; }

for r in mock trunk anvil; do echo "passed 43 of 43" >"$d/$r.result"; done
if sh "$script" --report >"$d/out" 2>&1; then fail "reported a pass with the services run missing"; fi
grep -q "^services: *did not run" "$d/out" || fail "the missing run is not named"
grep -q "NOT passed" "$d/out" || fail "the verdict does not say NOT passed"

echo "did not run: skipped in build_real: test_anvil (Skipped)" >"$d/services.result"
echo "passed 27 of 27" >"$d/services.result"
sh "$script" --report >"$d/out" 2>&1 || fail "four passes should be a pass"
[ "$(grep -c " - " "$d/out")" = 4 ] || fail "one line per run, each with what it proves"
grep -q "^anvil: *passed 43 of 43 - the real contracts" "$d/out" || fail "a run's line names the run, its count and what it proves"

echo "failed: 1 of 43 in build_trunk (see log)" >"$d/trunk.result"
if sh "$script" --report >"$d/out" 2>&1; then fail "reported a pass with the trunk run failed"; fi
grep -q "^trunk: *failed: 1 of 43" "$d/out" || fail "the failed run is not named"

echo "did not run: skipped in build_real: test_anvil (Skipped)" >"$d/trunk.result"
echo "passed 43 of 43" >"$d/trunk.result"
echo "did not run: skipped in build_real: test_anvil (Skipped) " >"$d/anvil.result"
if sh "$script" --report >"$d/out" 2>&1; then fail "reported a pass with the anvil run skipped"; fi
grep -q "^anvil: *did not run: skipped" "$d/out" || fail "a skipped suite is a run that did not happen"
echo "acceptance_report: the report refuses a pass with a run missing, skipped or failed, and passes four passes"
