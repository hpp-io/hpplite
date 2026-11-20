#!/bin/sh
# hpplite-sequencer stops when its database file was replaced under it (#379, somaro#352): the node finds the path names
# another file and posts nothing more (#320) - and the process stayed up, a healthy one to its supervisor, for as long
# as nobody read its stderr. `hpplite-sequencer <file>` exits 4 after the node's line, for a unit with
# Restart=on-failure to start it on the file there now; `--dir` says "<name>: error: ...", closes that one database
# and opens it again on the file there now, the others undisturbed.
#   tests/unit/sequencer_replaced.sh <path/to/hpplite-sequencer>
set -u
bin=$1
d=$(mktemp -d "${TMPDIR:-/tmp}/hpplite-seqrepl.XXXXXX")
pid=
trap '[ -n "$pid" ] && kill "$pid" 2>/dev/null; rm -rf "$d"' EXIT
fail() { echo "sequencer_replaced: FAIL: $1"; echo "--- stdout"; cat "$d/out" 2>/dev/null; echo "--- stderr"; cat "$d/err" 2>/dev/null; exit 1; }
command -v sqlite3 >/dev/null 2>&1 || { echo "sequencer_replaced: SKIP (no sqlite3)"; exit 77; }
key=0x0303030303030303030303030303030303030303030303030303030303030303
export HPPLITE_FILE_CHECK_SECONDS=1     # the file is compared with what was last sent every minute: here every second
waitline() {    # <file> <pattern> <tenths>: the line is there within the time
    i=0
    while [ $i -lt "$3" ]; do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.1; i=$((i + 1)); done
    return 1
}
# another program replaces the file: the old one moved aside (its inode kept by the sequencer), a new one made there
replace() {
    mv "$1" "$1.old"
    [ -f "$1-wal" ] && mv "$1-wal" "$1.old-wal"
    [ -f "$1-shm" ] && mv "$1-shm" "$1.old-shm"
    sqlite3 "$1" "CREATE TABLE restored(x); INSERT INTO restored VALUES(1)" || fail "make the new file"
}

# --- one file: the process exits 4 after the node's line
mkdir "$d/one"
export HPPLITE_PARAMS="l1=file://$d/one/chain&interval=200ms&datadir=$d/one&privkey=$key"
: >"$d/out"; : >"$d/err"          # emptied here, not by the background job: nothing the run before said is read (#385)
"$bin" "$d/one/app.db" >"$d/out" 2>"$d/err" &
pid=$!
waitline "$d/out" "synced height" 150 || fail "no 'synced height' line"
sqlite3 "$d/one/app.db" "CREATE TABLE t(x); INSERT INTO t VALUES(1)" || fail "a plain connection's write"
sleep 1
replace "$d/one/app.db"
i=0
while [ $i -lt 100 ] && kill -0 "$pid" 2>/dev/null; do sleep 0.1; i=$((i + 1)); done
grep -q "was replaced or removed under this sequencer" "$d/err" || fail "the node's line about the replaced file is not on stderr"
kill -0 "$pid" 2>/dev/null && fail "the sequencer goes on running after its file was replaced (10 s), posting nothing"
wait "$pid"; rc=$?; pid=
[ "$rc" = 4 ] || fail "the exit should be 4 after the file was replaced, not $rc"
[ "$(sqlite3 "$d/one/app.db" "SELECT count(*) FROM restored")" = 1 ] || fail "the file there now was touched by the stopping sequencer"
# the restart a supervisor makes: on the file there now
: >"$d/out"; : >"$d/err"          # emptied here, not by the background job: nothing the run before said is read (#385)
"$bin" "$d/one/app.db" >"$d/out" 2>"$d/err" &
pid=$!
waitline "$d/out" "synced height" 150 || fail "the restart on the file there now did not say it was synced"
kill -TERM "$pid"; wait "$pid" || fail "the restarted sequencer did not exit 0 on SIGTERM"; pid=
unset HPPLITE_PARAMS

# --- --dir: that one database is closed and opened again; the other is not disturbed
mkdir "$d/dir" "$d/a" "$d/b"
for n in a b; do
    ( umask 077; printf 'db=%s\nparams=contract=0x00000000000000000000000000000000000000a1&l1=file://%s/chain&interval=200ms&datadir=%s\nprivkey=%s\n' "$d/$n/app.db" "$d/$n" "$d/$n" "$key" > "$d/dir/$n.conf" )
done
: >"$d/out"; : >"$d/err"          # emptied here, not by the background job: nothing the run before said is read (#385)
"$bin" --dir "$d/dir" >"$d/out" 2>"$d/err" &
pid=$!
waitline "$d/out" "^a: synced height" 150 || fail "--dir: a did not open"
waitline "$d/out" "^b: synced height" 150 || fail "--dir: b did not open"
sqlite3 "$d/a/app.db" "CREATE TABLE t(x); INSERT INTO t VALUES(1)" || fail "--dir: a plain connection's write"
sleep 1
replace "$d/a/app.db"
waitline "$d/out" "^a: error: .*was replaced or removed under this sequencer" 100 || fail "--dir: no '<name>: error:' line for the replaced file"
i=0
while [ $i -lt 100 ] && [ "$(grep -c '^a: synced height' "$d/out")" -lt 2 ]; do sleep 0.1; i=$((i + 1)); done
[ "$(grep -c '^a: synced height' "$d/out")" = 2 ] || fail "--dir: the replaced database was not opened again on the file there now"
[ "$(grep -c '^b: synced height' "$d/out")" = 1 ] && ! grep -q "^b: closed" "$d/out" || fail "--dir: the other database was disturbed"
# the reopened one sequences the file there now: a write to it is posted
before=$(ls "$d/a/chain" | grep -c '^batch-[0-9]*\.bin$')
sqlite3 "$d/a/app.db" "INSERT INTO restored VALUES(2)" || fail "--dir: a write to the file there now"
i=0
while [ $i -lt 100 ] && [ "$(ls "$d/a/chain" | grep -c '^batch-[0-9]*\.bin$')" -le "$before" ]; do sleep 0.1; i=$((i + 1)); done
[ "$(ls "$d/a/chain" | grep -c '^batch-[0-9]*\.bin$')" -gt "$before" ] || fail "--dir: the reopened database posts nothing of the file there now"
kill -TERM "$pid"; wait "$pid" || fail "--dir: the stop did not exit 0"; pid=
echo "sequencer_replaced: ok"
