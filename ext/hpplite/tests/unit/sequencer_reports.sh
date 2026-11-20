#!/bin/sh
# hpplite-sequencer <file> says what a supervisor needs when nothing posts (#368, somaro#352): SIGUSR1 prints one line
# of the node's status on stderr, and commits pending for 30 s with nothing posted are said once, with the node's
# last error or that it gave none. Here the store does not answer (a port nothing listens on): every batch stays pending.
#   tests/unit/sequencer_reports.sh <path/to/hpplite-sequencer>
set -u
bin=$1
d=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-seqrep.XXXXXX")
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; [ -n "${KEEP:-}" ] && cp "$d/err" "$KEEP"; rm -rf "$d"' EXIT
fail() { echo "sequencer_reports: FAIL: $1"; echo "--- stdout"; cat "$d/out" 2>/dev/null; echo "--- stderr"; cat "$d/err" 2>/dev/null; exit 1; }
command -v sqlite3 >/dev/null 2>&1 || { echo "sequencer_reports: SKIP (no sqlite3)"; exit 77; }
export HPPLITE_PARAMS="l1=file://$d/chain&interval=200ms&datadir=$d&store=http://127.0.0.1:9&privkey=0x0202020202020202020202020202020202020202020202020202020202020202"
export HPPLITE_STUCK_SECONDS=3          # the 30 s, shortened for the test
export HPPLITE_STORE_FALLBACK=off       # the chain does not carry the bytes meanwhile: they stay pending
"$bin" "$d/app.db" >"$d/out" 2>"$d/err" &
pid=$!
i=0
while [ $i -lt 150 ] && ! grep -q "synced height" "$d/out" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
grep -q "synced height" "$d/out" || fail "no 'synced height' line"
sqlite3 "$d/app.db" "CREATE TABLE t(x); INSERT INTO t VALUES(1)" || fail "a plain connection's write"
sleep 1
kill -USR1 "$pid"
sleep 1
grep -q "hpplite-sequencer: $d/app.db status: height [0-9]* pending [1-9][0-9]* .*sequencer 1" "$d/err" || fail "SIGUSR1 printed no status line with the pending count"
sleep 4
grep -q "hpplite-sequencer: $d/app.db pending [1-9][0-9]* for [0-9]* s, nothing posted: " "$d/err" || fail "commits stuck past HPPLITE_STUCK_SECONDS were not said"
[ "$(grep -c 'nothing posted: ' "$d/err")" = 1 ] || fail "the stuck note should be said once, not at every check"
kill -TERM "$pid"; wait "$pid"; pid=

# SIGUSR1 while the open has not returned (#383): the handler was installed after it, and the signal's default ended
# the process (exit 158) - a supervisor asking "what are you doing?" of a sequencer slow to start killed it. Here the
# open waits on a store that accepts and does not answer; the question is kept and answered once the open returned.
command -v python3 >/dev/null 2>&1 || { echo "sequencer_reports: ok (no python3: SIGUSR1 during the open not run)"; exit 0; }
python3 -I -c '
import socket
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(16)
print(s.getsockname()[1], flush=True)
held = []
while True: held.append(s.accept()[0])
' > "$d/port" &
mute=$!
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; kill "$mute" 2>/dev/null; rm -rf "$d"' EXIT
i=0
while [ $i -lt 50 ] && ! [ -s "$d/port" ]; do sleep 0.1; i=$((i + 1)); done
[ -s "$d/port" ] || fail "the test's mute store did not start"
mkdir "$d/slow"
HPPLITE_PARAMS="l1=file://$d/slow/chain&interval=200ms&datadir=$d/slow&store=http://127.0.0.1:$(cat "$d/port")&privkey=0x0303030303030303030303030303030303030303030303030303030303030303" \
    "$bin" "$d/slow/app.db" >"$d/out" 2>"$d/err" &
pid=$!
sleep 2
[ -s "$d/out" ] && fail "the open returned with the store not answering: the test has no open to signal in"
kill -USR1 "$pid"
sleep 1
kill -0 "$pid" 2>/dev/null || { wait "$pid"; rc=$?; pid=; fail "SIGUSR1 before the open returned ended the process (exit $rc)"; }
kill "$mute" 2>/dev/null; wait "$mute" 2>/dev/null       # the store gone: the open returns
i=0
while [ $i -lt 300 ] && ! grep -q "hpplite-sequencer: $d/slow/app.db status: height " "$d/err" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
grep -q "hpplite-sequencer: $d/slow/app.db status: height " "$d/err" || fail "the SIGUSR1 sent during the open was not answered once it returned"
kill -TERM "$pid"; wait "$pid"; pid=
echo "sequencer_reports: ok"
