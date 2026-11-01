#!/bin/sh
# SQLite's own TCL tests, stock and with HPPLite on every connection (#62).
#   tests/sqlite_tcl/run.sh <work dir> <hpplite module (.dylib|.so)> test1 [test2 ...]
# Builds a stock testfixture from this tree in <work dir> if there is none
# (needs Tcl with its tclConfig.sh: TCL_CONFIG_DIR, default from tclsh), then
# runs each test/<name>.test both ways, each in its own directory, and prints
# per file: the summary line of each run and the tests that fail only with
# HPPLite. HPPLite runs with row capture (the default), no chain.
set -eu
work=$(mkdir -p "$1" && cd "$1" && pwd); module=$2; shift 2
here=$(cd "$(dirname "$0")" && pwd); root=$(cd "$here/../../../.." && pwd)
if [ ! -x "$work/testfixture" ]; then
    tcldir=${TCL_CONFIG_DIR:-$(echo 'puts [file dirname [file dirname [info nameofexecutable]]]/lib' | tclsh)}
    (cd "$work" && "$root/configure" --with-tcl="$tcldir" > configure.log 2>&1 && make -j8 testfixture > make.log 2>&1)
fi
# testfixture links Tcl dynamically
case $(uname) in Darwin) export DYLD_LIBRARY_PATH=${TCL_LIB_DIR:-${TCL_CONFIG_DIR:-}}${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH} ;; esac
for t in "$@"; do
    for v in stock hp; do
        d=$work/out/$v/$t
        # a fresh directory: tests leave read-only files behind (on macOS
        # `file attributes -readonly 1` sets the immutable flag)
        if [ -d "$d" ]; then
            chflags -R nouchg "$d" 2>/dev/null || true
            chmod -R u+w "$d"; rm -rf "${d:?}"
        fi
        mkdir -p "$d"; cd "$d"
        if [ $v = hp ]; then
            HPPLITE_MODULE=${module%.*} HPPLITE_FILES="*" HPPLITE_PARAMS="encrypt=off&interval=0" \
                "$work/testfixture" "$here/wrap.tcl" "$root/test/$t.test" > log 2>&1 || true
        else
            "$work/testfixture" "$here/wrap.tcl" "$root/test/$t.test" > log 2>&1 || true
        fi
        grep -E "errors out of" log | tail -1 | sed -E 's/ on .*//' > summary || true
        # every failed test: the closing list has those that threw as well
        # as those that compared unequal ("! name"); a run that stopped part
        # way has no closing list, only the latter
        if grep -q "^!Failures on these tests:" log; then
            grep "^!Failures on these tests:" log | tail -1 | sed -E 's/^!Failures on these tests: *//' | tr ' ' '\n' | grep . | sort -u > failed
        else
            grep -E "^! " log | sed -E 's/^! ([^ ]+).*/\1/' | sort -u > failed || true
        fi
    done
    s=$(cat "$work/out/stock/$t/summary"); h=$(cat "$work/out/hp/$t/summary")
    new=$(comm -13 "$work/out/stock/$t/failed" "$work/out/hp/$t/failed" | tr '\n' ' ')
    printf '%-16s stock: %-26s hpplite: %-26s only with hpplite: %s\n' "$t" "${s:-(aborted)}" "${h:-(aborted)}" "$new"
done
