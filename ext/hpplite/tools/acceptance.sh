#!/bin/sh
# The acceptance run (#258): the four test runs a change must pass before it is
# committed, in one command, each stating what it proves - and a refusal to
# report a pass while any of them did not happen (a build not configured,
# foundry missing, a suite that skipped itself).
#
#   tools/acceptance.sh [run ...]     the runs named, default all four, then the report
#   tools/acceptance.sh --report      the report of the last results alone
#
# The runs, in order:
#   mock      ctest on the mock-chain build                  (every suite, no network)
#   trunk     the same ctest on a build against SQLite trunk (doc/sqlite-dependencies.md holds upstream)
#   anvil     test_anvil on the real-L1 build                (the real contracts on a local chain)
#   services  the rest of the real-L1 build's ctest          (the reference store and relay, x402, encodings)
#
# Build directories (configured here when missing, where the inputs are known):
#   HPPLITE_BUILD_MOCK    default ext/hpplite/build_mock
#   HPPLITE_BUILD_TRUNK   default ext/hpplite/build_trunk; needs HPPLITE_TRUNK_SQLITE, the directory holding a
#                         sqlite3.c built from SQLite trunk (make sqlite3.c in a trunk checkout's build dir)
#   HPPLITE_BUILD_REAL    default ext/hpplite/build_real; configured with -DHPPLITE_USE_REAL_L1=ON and
#                         -DHPPLITE_SQLITE_SRC=$HPPLITE_SQLITE_SRC (default <repo>/build, the repo's own amalgamation)
# Results: $HPPLITE_ACCEPTANCE_DIR/<run>.result (default ext/hpplite/build_acceptance), with each run's
# ctest output beside it. Exit 0 only when all four results are passes.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$here/../.." && pwd)
dir=${HPPLITE_ACCEPTANCE_DIR:-$here/build_acceptance}
mock=${HPPLITE_BUILD_MOCK:-$here/build_mock}
trunk=${HPPLITE_BUILD_TRUNK:-$here/build_trunk}
real=${HPPLITE_BUILD_REAL:-$here/build_real}
jobs=${HPPLITE_ACCEPTANCE_JOBS:-4}
runs="mock trunk anvil services"

proves() {
    case $1 in
        mock)     echo "every suite on the mock chain: node, durability, page capture, store, keys, crash, docs" ;;
        trunk)    echo "the same suites on SQLite trunk: what the capture relies on in SQLite (doc/sqlite-dependencies.md) still holds upstream" ;;
        anvil)    echo "the real contracts on a local chain: leases, anchors, stores, relays, snapshots, every scenario" ;;
        services) echo "the reference store and relay, the x402 conformance vectors and the chain client's encodings, on the real-L1 build" ;;
    esac
}

configure() {   # <run> <build dir>: configure a missing build when its inputs are known; 0 configured
    case $1 in
        mock)  cmake -S "$here" -B "$2" -DCMAKE_BUILD_TYPE=Release >"$dir/$1.configure.log" 2>&1 ;;
        trunk) [ -n "${HPPLITE_TRUNK_SQLITE:-}" ] || { echo "did not run: $2 is not configured and HPPLITE_TRUNK_SQLITE (a trunk sqlite3.c's directory) is not set" >"$dir/$1.result"; return 1; }
               cmake -S "$here" -B "$2" -DCMAKE_BUILD_TYPE=Release -DHPPLITE_SQLITE_SRC="$HPPLITE_TRUNK_SQLITE" >"$dir/$1.configure.log" 2>&1 ;;
        *)     cmake -S "$here" -B "$2" -DCMAKE_BUILD_TYPE=Release -DHPPLITE_USE_REAL_L1=ON -DHPPLITE_SQLITE_SRC="${HPPLITE_SQLITE_SRC:-$repo/build}" >"$dir/$1.configure.log" 2>&1 ;;
    esac || { echo "did not run: configuring $2 failed (see $dir/$1.configure.log)" >"$dir/$1.result"; return 1; }
}

run_ctest() {   # <run> <build dir> [ctest args...]: builds, runs, writes the result
    name=$1; bdir=$2; shift 2
    rm -f "$dir/$name.result"
    if [ ! -f "$bdir/CMakeCache.txt" ]; then configure "$name" "$bdir" || return; fi
    if ! cmake --build "$bdir" -j "$jobs" >"$dir/$name.build.log" 2>&1; then
        echo "did not run: the build in $bdir failed (see $dir/$name.build.log)" >"$dir/$name.result"; return
    fi
    (cd "$bdir" && ctest -j "$jobs" --output-on-failure "$@") >"$dir/$name.log" 2>&1
    summary=$(grep -E "tests passed, .* tests failed out of" "$dir/$name.log" | tail -1)
    total=$(printf '%s' "$summary" | sed -n 's/.*out of \([0-9]*\).*/\1/p')
    failed=$(printf '%s' "$summary" | sed -n 's/.*passed, \([0-9]*\) tests failed.*/\1/p')
    skipped=$(grep -E "\(Skipped\)|\(Not Run\)" "$dir/$name.log" | sed 's/^[[:space:]]*[0-9]* - //' | tr '\n' ' ')
    if [ -z "$total" ] || [ "$total" = 0 ]; then
        echo "did not run: ctest in $bdir ran nothing (see $dir/$name.log)" >"$dir/$name.result"
    elif [ -n "$skipped" ]; then
        echo "did not run: skipped in $bdir: $skipped(see $dir/$name.log)" >"$dir/$name.result"
    elif [ "${failed:-1}" != 0 ]; then
        echo "failed: $failed of $total in $bdir (see $dir/$name.log)" >"$dir/$name.result"
    else
        echo "passed $total of $total" >"$dir/$name.result"
    fi
}

report() {
    rc=0
    for r in $runs; do
        if [ -f "$dir/$r.result" ]; then result=$(cat "$dir/$r.result"); else result="did not run (no result in $dir)"; fi
        case $result in passed*) ;; *) rc=1 ;; esac
        printf '%-9s %s - %s\n' "$r:" "$result" "$(proves "$r")"
    done
    [ $rc = 0 ] && echo "acceptance: all four runs passed" || echo "acceptance: NOT passed (a run above did not pass, or did not happen)"
    return $rc
}

mkdir -p "$dir"
if [ "${1:-}" = "--report" ]; then report; exit $?; fi
wanted=${*:-$runs}
for r in $wanted; do
    case $r in
        mock)     run_ctest mock "$mock" ;;
        trunk)    run_ctest trunk "$trunk" ;;
        anvil)    run_ctest anvil "$real" -R '^test_anvil$' ;;
        services) run_ctest services "$real" -E '^test_anvil$' ;;
        *) echo "acceptance: no run named '$r' (mock, trunk, anvil, services)" >&2; exit 2 ;;
    esac
    printf '%-9s %s\n' "$r:" "$(cat "$dir/$r.result")"
done
report
