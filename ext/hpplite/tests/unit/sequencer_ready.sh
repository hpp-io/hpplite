#!/bin/sh
# hpplite-sequencer says when the file is synced (#264): one line on stdout right after open, before anything
# else - "hpplite-sequencer: <file> synced height N" - so a supervisor restoring an empty volume can wait for the
# rebuild before the application opens the file. The line is there while the process runs (unbuffered), and N is
# the height hpplite_status has: one more after a plain connection's write was posted at the stop.
#   tests/unit/sequencer_ready.sh <path/to/hpplite-sequencer>
set -u
bin=$1
d=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-seqready.XXXXXX")
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$d"' EXIT
fail() { echo "sequencer_ready: FAIL: $1"; echo "--- stdout"; cat "$d/out" 2>/dev/null; echo "--- stderr"; cat "$d/err" 2>/dev/null; exit 1; }
# a key, so the batches are posted (without one they stay pending); the timer posts the plain write
export HPPLITE_PARAMS="l1=file://$d/chain&interval=200ms&datadir=$d&privkey=0x0101010101010101010101010101010101010101010101010101010101010101"

# starts: the sequencer on a fresh file; the line must appear while it runs
start() {
    "$bin" "$d/app.db" >"$d/out" 2>"$d/err" &
    pid=$!
    i=0
    while [ $i -lt 150 ]; do
        grep -q "synced height" "$d/out" 2>/dev/null && return 0
        kill -0 "$pid" 2>/dev/null || fail "the sequencer exited before saying it was synced"
        sleep 0.1; i=$((i + 1))
    done
    fail "no 'synced height' line on stdout within 15 s of the start"
}
stop() { kill -TERM "$pid"; wait "$pid" || fail "the sequencer did not exit 0 on SIGTERM"; pid=; }

height() { head -1 "$d/out" | sed -n "s|^hpplite-sequencer: $d/app.db synced height \([0-9][0-9]*\)\$|\1|p"; }

start
h0=$(height)
[ -n "$h0" ] || fail "the first stdout line should be 'hpplite-sequencer: <file> synced height N'"
if command -v sqlite3 >/dev/null 2>&1; then
    sqlite3 "$d/app.db" "CREATE TABLE t(x); INSERT INTO t VALUES(1)" || fail "a plain connection's write"
    sleep 1                                # the timer posts what the plain connection wrote: one more batch
    stop
    start
    h1=$(height)
    [ "$h1" = "$((h0 + 1))" ] || fail "after a posted write the line should say height $((h0 + 1)) (what hpplite_status says), not '$h1'"
fi
stop
echo "sequencer_ready: ok"
