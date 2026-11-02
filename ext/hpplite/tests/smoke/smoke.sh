#!/bin/sh
# Real-app smoke test (issue #27): drive the loadable module from Python's
# sqlite3 and from the sqlite3 command-line shell, the two ways people who
# are not us will first touch HPPLite. Exit 77 = skipped (no usable tool).
#
#   tests/smoke/smoke.sh /path/to/hpplite.dylib
set -eu
ext=$1
here=$(cd "$(dirname "$0")" && pwd)
dir=${TMPDIR:-/tmp}/hpplite_smoke.$$
mkdir -p "$dir"
trap 'rm -rf "$dir"' EXIT
ran=0

# Python: needs a build with enable_load_extension (macOS system python lacks it)
for py in python3 /opt/homebrew/bin/python3 /usr/local/bin/python3 /usr/bin/python3; do
    command -v "$py" >/dev/null 2>&1 || continue
    "$py" -c 'import sqlite3; sqlite3.connect(":memory:").enable_load_extension(True)' >/dev/null 2>&1 || continue
    "$py" "$here/smoke_py.py" "$ext" "$dir" 2> "$dir/py.err" || { cat "$dir/py.err"; exit 1; }
    # Python clears a connection's callbacks as it closes it: not a takeover
    if grep -q "replaced the connection" "$dir/py.err"; then cat "$dir/py.err"; echo "python: false hooks_replaced at close"; exit 1; fi
    ran=1
    break
done

# sqlite3 shell: .load, URI open, status, flush, close, reopen
# (Apple's /usr/bin/sqlite3 is built without extension loading: skip it)
if command -v sqlite3 >/dev/null 2>&1 \
   && ! sqlite3 :memory: ".load /nonexistent/module" 2>&1 | grep -q "unknown command"; then
    key=0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20
    uri="file:$dir/cli.db?hpplite=on&l1=local&privkey=0x$key&datadir=$dir"
    mod=${ext%.*}
    out=$(sqlite3 -batch -bail <<SQL
.load $mod
.open '$uri'
CREATE TABLE t(id INTEGER PRIMARY KEY, v);
INSERT INTO t(v) VALUES('a'),('b'),('c');
SELECT 'pending=' || pending FROM hpplite_status;
SELECT 'flush=' || hpplite_flush();
SELECT 'height=' || height || ',pending=' || pending FROM hpplite_status;
INSERT INTO t(v) VALUES('d');
.quit
SQL
)
    echo "$out" | tr '\n' ' '
    # the CREATE and three rows (row capture, #19); with capture=text the CREATE and the INSERT
    want=4; [ "${HPPLITE_CAPTURE:-}" = text ] && want=2
    [ "${HPPLITE_CAPTURE:-}" = pages ] && want=3            # the file as opened and two commits (page capture, #146)
    echo "$out" | grep -q "^pending=$want\$" || { echo "cli: expected pending=$want"; exit 1; }
    echo "$out" | grep -q '^flush=1$' || { echo "cli: expected flush=1"; exit 1; }
    echo "$out" | grep -q '^height=2,pending=0$' || { echo "cli: expected height 2"; exit 1; }
    again=$(sqlite3 -batch "$uri" ".load $mod" "SELECT height || ',' || pending || ',' || (SELECT count(*) FROM t) FROM hpplite_status;")
    [ "$again" = "3,0,4" ] || { echo "cli: reopen expected 3,0,4 got $again"; exit 1; }
    echo "cli smoke: ok"
    ran=1
fi

[ "$ran" = 1 ] || { echo "smoke: no python3 with load_extension and no sqlite3 shell; skipped"; exit 77; }
