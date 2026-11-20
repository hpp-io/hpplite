#!/bin/sh
# hpplite-sequencer says "synced height N" only when the node is synced (#381, somaro): the line came when the open
# had returned, also after a sync that stopped - a file restored onto a rollup with a history, the app's connection
# open in WAL: 'batch 1: pages cannot be applied while other connections hold the file in WAL mode' on stderr and
# "synced height 1" on stdout, a supervisor waiting on the line told the file was the rollup's state. The line is
# held until hpplite_status.synced is 1; meanwhile "not synced: <the node's error>" is on stderr and the process
# stays up; `--dir` says "<name>: not synced: ..." in place of "<name>: synced height N", and that line when it is.
#   tests/unit/sequencer_synced.sh <path/to/hpplite-sequencer>
set -u
bin=$1
d=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-seqsync.XXXXXX")
pid=
app=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; [ -n "$app" ] && kill "$app" 2>/dev/null; rm -rf "$d"' EXIT
fail() { echo "sequencer_synced: FAIL: $1"; echo "--- stdout"; cat "$d/out" 2>/dev/null; echo "--- stderr"; cat "$d/err" 2>/dev/null; exit 1; }
command -v sqlite3 >/dev/null 2>&1 || { echo "sequencer_synced: SKIP (no sqlite3)"; exit 77; }
key=0x0404040404040404040404040404040404040404040404040404040404040404
waitline() {    # <file> <pattern> <tenths>: the line is there within the time
    i=0
    while [ $i -lt "$3" ]; do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; i=$((i + 1)); done
    return 1
}
# a rollup with a history: a host sequences three commits and stops
history() {     # <dir>
    mkdir "$1/h1"
    : >"$d/out"; : >"$d/err"          # emptied here, not by the background job: nothing the run before said is read (#385)
    HPPLITE_PARAMS="l1=file://$1/chain&interval=200ms&datadir=$1/h1&privkey=$key" "$bin" "$1/h1/app.db" >"$d/out" 2>"$d/err" &
    pid=$!
    waitline "$d/out" "synced height 1" 150 || fail "the first host did not say it was synced"
    for v in 1 2 3; do
        sqlite3 "$1/h1/app.db" "BEGIN; CREATE TABLE IF NOT EXISTS t(v); INSERT INTO t VALUES($v); COMMIT" || fail "the first host's app could not write"
        sleep 0.5
    done
    kill -TERM "$pid"; wait "$pid" || fail "the first host did not exit 0 on SIGTERM"; pid=
    [ "$(ls "$1/chain" | grep -c '^batch-[0-9]*\.bin$')" -ge 1 ] || fail "the first host posted nothing"
}
# a restored file on the next host, the app's connection to it open in WAL: larger than the history's file
restored() {    # <dir>
    mkdir "$1/h2"
    mkfifo "$1/fifo"
    sqlite3 "$1/h2/app.db" < "$1/fifo" > "$1/appout" 2>&1 &
    app=$!
    exec 9>"$1/fifo"
    echo "PRAGMA journal_mode=WAL; CREATE TABLE mine(v); WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c WHERE i<2000) INSERT INTO mine SELECT hex(randomblob(40)) FROM c; SELECT 'held';" >&9
    waitline "$1/appout" "held" 100 || fail "the app's connection did not open the restored file"
}
appcloses() { echo ".quit" >&9; exec 9>&-; wait "$app"; app=; }

# --- one file: no "synced" line after the sync that stopped; "not synced" on stderr; the process up; the line when it is
mkdir "$d/one"
history "$d/one"
restored "$d/one"
: >"$d/out"; : >"$d/err"          # emptied here, not by the background job: nothing the run before said is read (#385)
HPPLITE_PARAMS="l1=file://$d/one/chain&interval=200ms&datadir=$d/one/h2&privkey=$key" "$bin" "$d/one/h2/app.db" >"$d/out" 2>"$d/err" &
pid=$!
waitline "$d/err" "pages cannot be applied while other connections hold the file in WAL mode" 150 || fail "the node did not say why the history was not applied"
sleep 2
grep -q "synced height" "$d/out" && fail "'synced height' on stdout after a sync that stopped: the file is not the rollup's state"
grep -q "^hpplite-sequencer: $d/one/h2/app.db not synced: .*pages cannot be applied" "$d/err" || fail "no 'hpplite-sequencer: <file> not synced: <the node's error>' line on stderr"
kill -0 "$pid" 2>/dev/null || fail "the sequencer exited: it stays up and says the line when the node is synced"
appcloses
waitline "$d/out" "synced height" 150 || fail "the node synced once the app's connection closed, and the line did not come"
head -1 "$d/out" | grep -q "^hpplite-sequencer: $d/one/h2/app.db synced height [0-9][0-9]*\$" || fail "the first stdout line should be 'hpplite-sequencer: <file> synced height N'"
[ "$(head -1 "$d/out" | sed 's/.* //')" -ge 2 ] || fail "synced at height 1 on a rollup with a history"
[ "$(grep -c 'synced height' "$d/out")" = 1 ] || fail "the synced line is said once"
kill -TERM "$pid"; wait "$pid" || fail "the sequencer did not exit 0 on SIGTERM"; pid=
[ "$(sqlite3 "$d/one/h2/app.db" "SELECT count(*) FROM t")" = 3 ] || fail "the file said to be synced does not hold the history's rows"

# --- --dir: "<name>: not synced: ..." in place of "<name>: synced height N", and that line when it is
mkdir "$d/many" "$d/many/dir"
history "$d/many"
restored "$d/many"
( umask 077; printf 'db=%s\nparams=contract=0x00000000000000000000000000000000000000a1&l1=file://%s/chain&interval=200ms&datadir=%s\nprivkey=%s\n' "$d/many/h2/app.db" "$d/many" "$d/many/h2" "$key" > "$d/many/dir/a.conf" )
: >"$d/out"; : >"$d/err"          # emptied here, not by the background job: nothing the run before said is read (#385)
"$bin" --dir "$d/many/dir" >"$d/out" 2>"$d/err" &
pid=$!
waitline "$d/out" "^a: not synced: .*pages cannot be applied" 150 || fail "--dir: no '<name>: not synced: <the node's error>' line"
sleep 2
grep -q "^a: synced height" "$d/out" && fail "--dir: '<name>: synced height' after a sync that stopped"
appcloses
waitline "$d/out" "^a: synced height [0-9]" 150 || fail "--dir: the node synced once the app's connection closed, and the line did not come"
kill -TERM "$pid"; wait "$pid" || fail "--dir: the stop did not exit 0"; pid=
echo "sequencer_synced: ok"
