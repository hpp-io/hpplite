#!/bin/sh
# Run tests/bench/oltp_bench.c against the loadable extension of each tag (#140).
#   tools/bench_versions.sh [-o outdir] [-s seconds] [-r runs] [tag|path ...]
# Each argument is a git tag (built in a worktree, Release, hpplite_ext only) or
# the path of an already built hpplite.dylib/.so ("tip" is this tree's build_mock).
# Writes <outdir>/<label>.jsonl (one line per run and configuration) and
# <outdir>/summary.md (the median run of each configuration).
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(cd "$here/../.." && pwd)
out=${BENCH_OUT:-$here/build_mock/bench}
seconds=10
runs=3
sqlite_src=${HPPLITE_SQLITE_SRC:-$root/build}
while getopts "o:s:r:" o; do case $o in o) out=$OPTARG;; s) seconds=$OPTARG;; r) runs=$OPTARG;; *) exit 2;; esac; done
shift $((OPTIND - 1))
[ $# -gt 0 ] || set -- v0.1.0 v0.1.1 v0.2.0 v0.3.0 tip
bench=$here/build_mock/oltp_bench
[ -x "$bench" ] || cmake --build "$here/build_mock" -t oltp_bench
mkdir -p "$out"
wt=${BENCH_WORKTREES:-$out/worktrees}
ext_of() {
    case $1 in
        tip) cmake --build "$here/build_mock" -t hpplite_ext >/dev/null; echo "$here/build_mock/hpplite.dylib";;
        */*) echo "$1";;
        *)  d=$wt/$1
            [ -d "$d" ] || git -C "$root" worktree add -q "$d" "$1"
            b=$d/ext/hpplite/build_bench
            [ -f "$b/hpplite.dylib" ] || {
                cmake -S "$d/ext/hpplite" -B "$b" -DCMAKE_BUILD_TYPE=Release -DHPPLITE_USE_REAL_L1=ON \
                      -DHPPLITE_BUILD_TESTS=OFF -DHPPLITE_BUILD_EXAMPLES=OFF -DHPPLITE_SQLITE_SRC="$sqlite_src" >/dev/null
                cmake --build "$b" -t hpplite_ext >/dev/null
            }
            echo "$b/hpplite.dylib";;
    esac
}
# configurations: mode trx
confs="read_write:trx write_only:trx read_write:auto write_only:auto read_only:trx"
bench_run() { # label extarg...
    label=$1; shift
    for conf in $confs; do
        mode=${conf%%:*}; trx=${conf##*:}
        skip=""; [ "$trx" = auto ] && skip="--skip-trx"
        i=0
        while [ $i -lt "$runs" ]; do
            "$bench" "$@" --mode "$mode" $skip --seconds "$seconds" --dir "$out/scratch" --label "$label" --json "$out/$label.jsonl"
            i=$((i + 1))
        done
    done
}
: > "$out/summary.md"
rm -f "$out/plain.jsonl"; bench_run plain
for v in "$@"; do
    ext=$(ext_of "$v")
    label=$(basename "$v" .dylib); [ "$v" = tip ] && label=tip; case $v in v*) label=$v;; esac
    echo "== $label: $ext"
    rm -f "$out/$label.jsonl"; bench_run "$label" --ext "$ext"
done
python3 - "$out" plain "$@" <<'PY' >> "$out/summary.md"
import json, sys, os, statistics
out = sys.argv[1]; labels = sys.argv[2:]
labels = [l if (l == 'tip' or l.startswith('v')) else os.path.basename(l).replace('.dylib','') for l in labels]
rows = {}
for l in labels:
    for line in open(f"{out}/{l}.jsonl"):
        r = json.loads(line); rows.setdefault((l, r['mode'], r['trx']), []).append(r)
def med(rs, k): return statistics.median(x[k] for x in rs)
confs = [('read_write', True), ('write_only', True), ('read_write', False), ('write_only', False), ('read_only', True)]
for mode, trx in confs:
    print(f"\n### {mode}, {'one transaction each' if trx else 'autocommit'}\n")
    print("| version | tps | stmt/s | p50 ms | p95 ms | p99 ms | max ms | batches | flush ms | db MB |")
    print("|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|")
    for l in labels:
        rs = rows.get((l, mode, trx))
        if not rs: continue
        print(f"| {l} | {med(rs,'tps'):.0f} | {med(rs,'stmts_per_s'):.0f} | {med(rs,'lat_p50_ms'):.3f} | {med(rs,'lat_p95_ms'):.3f} | "
              f"{med(rs,'lat_p99_ms'):.3f} | {med(rs,'lat_max_ms'):.1f} | {med(rs,'batches'):.0f} | {med(rs,'flush_ms'):.0f} | {med(rs,'db_bytes')/1048576:.1f} |")
PY
cat "$out/summary.md"
