#!/bin/sh
# hpplite-sequencer stops, with an exit of its own, on a database file that is not at the root its bookkeeping holds
# (#427). The node refuses such a file (#425: a node killed while it wrote a batch's pages; a file restored beside its
# sidecar - the shape made here), tries no batch on it and says so at every sync - and the process stayed up, a healthy
# one to its supervisor, and a restart met the same file. `hpplite-sequencer <file>` exits 5 after the node's line and
# "... stopping (exit 5): ..."; `--once` likewise; `--dir` says "<name>: error: ...", closes that one database and does
# not open it again until the directory is read again (SIGHUP), the others undisturbed. The file set aside with its
# sidecar, the next start rebuilds it from the history. And an open that fails says the node's reason with the tool's
# "cannot open" line, which said only "(HPPLITE_PARAMS?)".
#   tests/unit/sequencer_off_root.sh <path/to/hpplite-sequencer>
set -u
bin=$1
d=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-seqroot.XXXXXX")
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$d"' EXIT
fail() { echo "sequencer_off_root: FAIL: $1"; echo "--- stdout"; cat "$d/out" 2>/dev/null; echo "--- stderr"; sort "$d/err" 2>/dev/null | uniq -c; exit 1; }
command -v sqlite3 >/dev/null 2>&1 || { echo "sequencer_off_root: SKIP (no sqlite3)"; exit 77; }
key=0x0505050505050505050505050505050505050505050505050505050505050505
waitline() {    # <file> <pattern> <tenths>: the line is there within the time
    i=0
    while [ $i -lt "$3" ]; do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; i=$((i + 1)); done
    return 1
}
batches() { ls "$1/chain" 2>/dev/null | grep -c '^batch-[0-9]*\.bin$'; }
params() { echo "contract=0x00000000000000000000000000000000000000a1&l1=file://$1/chain&interval=200ms&datadir=$1"; }
round() {       # <dir> <sql>: a sequencer up on <dir>/app.db, the write posted, the sequencer stopped
    : >"$d/out"; : >"$d/err"          # emptied here, not by the background job: nothing the run before said is read (#385)
    HPPLITE_PARAMS="$(params "$1")&privkey=$key" "$bin" "$1/app.db" >"$d/out" 2>"$d/err" &
    pid=$!
    waitline "$d/out" "synced height" 150 || fail "making the history: no 'synced height' line"
    was=$(batches "$1")
    sqlite3 "$1/app.db" "$2" || fail "making the history: a plain connection's write"
    i=0
    while [ $i -lt 100 ] && [ "$(batches "$1")" -le "$was" ]; do sleep 0.1; i=$((i + 1)); done
    [ "$(batches "$1")" -gt "$was" ] || fail "making the history: the write was not posted"
    kill -TERM "$pid"; wait "$pid" || fail "making the history: the stop did not exit 0"; pid=
}
# a history of three writes in <dir>/chain; then the file of the first beside the bookkeeping of the second: the next
# start has the third to apply, on a file that is not what its bookkeeping holds
off_root() {
    mkdir "$1"
    round "$1" "CREATE TABLE t(k INTEGER PRIMARY KEY, z); INSERT INTO t VALUES(1, printf('%03000d', 1))"
    cp "$1/app.db" "$1/first.db"
    round "$1" "UPDATE t SET z = printf('%03000d', 2)"
    cp "$1/app.db-hpplite.db" "$1/second-hpplite.db"
    round "$1" "UPDATE t SET z = printf('%03000d', 3)"
    rm -f "$1/app.db-wal" "$1/app.db-shm" "$1/app.db-hpplite.db-wal" "$1/app.db-hpplite.db-shm"
    cp "$1/first.db" "$1/app.db"; cp "$1/second-hpplite.db" "$1/app.db-hpplite.db"
}
aside() {       # the remedy: the file and its sidecar set aside
    mkdir "$1/aside"
    for f in app.db app.db-wal app.db-shm app.db-hpplite.db app.db-hpplite.db-wal app.db-hpplite.db-shm; do [ -e "$1/$f" ] && mv "$1/$f" "$1/aside/$f"; done
    return 0
}

# --- one file: the process exits 5 after the node's line
off_root "$d/one"
export HPPLITE_PARAMS="$(params "$d/one")&privkey=$key"
n0=$(batches "$d/one")
: >"$d/out"; : >"$d/err"
"$bin" "$d/one/app.db" >"$d/out" 2>"$d/err" &
pid=$!
i=0
while [ $i -lt 100 ] && kill -0 "$pid" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
grep -q "the database file is not at the root its bookkeeping" "$d/err" || fail "the node's line about the file is not on stderr"
kill -0 "$pid" 2>/dev/null && fail "the sequencer goes on running on a file that is not at the root its bookkeeping holds (10 s), refusing it at every sync"
wait "$pid"; rc=$?; pid=
[ "$rc" = 5 ] || fail "the exit should be 5 on a file that is not at the root its bookkeeping holds, not $rc"
grep -q "^hpplite-sequencer: .* stopping (exit 5): " "$d/err" || fail "no 'stopping (exit 5)' line on stderr"
grep -q "synced height" "$d/out" && fail "the sequencer said the file was synced"
[ "$(batches "$d/one")" = "$n0" ] || fail "the sequencer posted from the refused file"
# --once meets the same file
: >"$d/out"; : >"$d/err"
"$bin" --once "$d/one/app.db" >"$d/out" 2>"$d/err"; rc=$?
[ "$rc" = 5 ] || fail "--once: the exit should be 5, not $rc"
grep -q "^hpplite-sequencer: .* stopping (exit 5): " "$d/err" || fail "--once: no 'stopping (exit 5)' line on stderr"
[ "$(batches "$d/one")" = "$n0" ] || fail "--once posted from the refused file"
# what the line says to do: the file and its sidecar set aside, the start rebuilds the file from the history
aside "$d/one"
: >"$d/out"; : >"$d/err"
"$bin" "$d/one/app.db" >"$d/out" 2>"$d/err" &
pid=$!
waitline "$d/out" "synced height" 150 || fail "the start on an empty file, the refused one set aside, did not say it was synced"
kill -TERM "$pid"; wait "$pid" || fail "the sequencer on the rebuilt file did not exit 0 on SIGTERM"; pid=
[ "$(sqlite3 "$d/one/app.db" "SELECT CAST(z AS INTEGER) FROM t")" = 3 ] || fail "the rebuilt file is not the history's last state"

# --- a file the node cannot read: the tool's line says the node's reason
mkdir "$d/bad"
head -c 8192 /dev/zero | tr '\0' 'x' > "$d/bad/app.db"
export HPPLITE_PARAMS="$(params "$d/bad")&privkey=$key"
: >"$d/out"; : >"$d/err"
"$bin" "$d/bad/app.db" >"$d/out" 2>"$d/err"; rc=$?
[ "$rc" = 1 ] || fail "a file that is not a database: the exit should be 1, not $rc"
grep -q "^hpplite-sequencer: cannot open .*cannot read the database" "$d/err" || fail "a file that is not a database: the 'cannot open' line does not say the node's reason"
unset HPPLITE_PARAMS

# --- --dir: that one database is closed and not opened again; the other is not disturbed
off_root "$d/a"
mkdir "$d/dir" "$d/b"
for n in a b; do
    ( umask 077; printf 'db=%s\nparams=%s\nprivkey=%s\n' "$d/$n/app.db" "$(params "$d/$n")" "$key" > "$d/dir/$n.conf" )
done
: >"$d/out"; : >"$d/err"
"$bin" --dir "$d/dir" >"$d/out" 2>"$d/err" &
pid=$!
waitline "$d/out" "^b: synced height" 150 || fail "--dir: b did not open"
waitline "$d/out" "^a: not synced: the database file is not at the root its bookkeeping" 150 || fail "--dir: no '<name>: not synced:' line with the node's reason"
waitline "$d/out" "^a: error: its file is not at the root its bookkeeping holds: not opened again" 100 || fail "--dir: the database on the refused file is not closed with a line that says so"
sleep 2
grep -q "^a: synced height" "$d/out" && fail "--dir: the refused file was said to be synced"
[ "$(grep -c '^a: not synced' "$d/out")" = 1 ] || fail "--dir: the refused file was opened again by itself"
aside "$d/a"
kill -HUP "$pid"
waitline "$d/out" "^a: synced height" 150 || fail "--dir: after the file was set aside and SIGHUP, the database did not open on an empty file"
[ "$(grep -c '^b: synced height' "$d/out")" = 1 ] && ! grep -q "^b: closed" "$d/out" || fail "--dir: the other database was disturbed"
kill -TERM "$pid"; wait "$pid" || fail "--dir: the stop did not exit 0"; pid=
[ "$(sqlite3 "$d/a/app.db" "SELECT CAST(z AS INTEGER) FROM t")" = 3 ] || fail "--dir: the rebuilt file is not the history's last state"
echo "sequencer_off_root: ok"
