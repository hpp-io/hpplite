"""HPPLite through Python's sqlite3 module and a loadable hpplite module.

Usage: smoke_py.py <path/to/hpplite.(dylib|so)> <scratch dir>
Exercises the README claims an outside program relies on: load_extension,
URI open, implicit transactions, hpplite_status, hpplite_flush(), close
flushes, reopen continues. Exit status is the verdict.
"""
import sqlite3, sys, os

ext, d = sys.argv[1], sys.argv[2]
key = "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20"
uri = f"file:{d}/py.db?hpplite=on&l1=local&privkey=0x{key}&datadir={d}"
for f in os.listdir(d):
    if f.startswith("py.db"):
        os.unlink(os.path.join(d, f))

c = sqlite3.connect(":memory:")
c.enable_load_extension(True)
c.load_extension(os.path.splitext(ext)[0])      # SQLite adds the platform suffix
c.close()

c = sqlite3.connect(uri, uri=True, isolation_level="DEFERRED")
cols = [r[1] for r in c.execute("PRAGMA table_info(hpplite_status)")]
assert cols[:2] == ["height", "pending"], cols
c.execute("CREATE TABLE notes(id INTEGER PRIMARY KEY, body TEXT, n INTEGER)")
c.executemany("INSERT INTO notes(body, n) VALUES(?, ?)", [(f"note {i}", i) for i in range(300)])
c.commit()
c.execute("UPDATE notes SET body = body || '!' WHERE id % 7 = 0")
c.commit()
# a changed row per log row (row capture, #19); with capture=text a statement:
# 300 inserted, 42 updated (id % 7 = 0), and the CREATE TABLE
cap = os.environ.get("HPPLITE_CAPTURE", "").lower()
pending = c.execute("SELECT pending FROM hpplite_status").fetchone()[0]
if cap in ("pages", "", "auto"):
    assert pending in (3, 4), pending       # three commits, and the file as opened when it had a page (#146): the default
else:
    assert pending == (302 if cap == "text" else 343), pending
h = c.execute("SELECT hpplite_flush()").fetchone()[0]       # the last height posted (40 KB batches: maybe 2)
assert h >= 1, h
assert c.execute("SELECT height, pending FROM hpplite_status").fetchone() == (h + 1, 0)
# a rolled-back transaction leaves no trace
c.execute("INSERT INTO notes(body, n) VALUES('never', -1)")
c.rollback()
assert c.execute("SELECT pending FROM hpplite_status").fetchone()[0] == 0
c.execute("DELETE FROM notes WHERE id > 290")
c.commit()
c.close()                                          # flushes batch 2

c = sqlite3.connect(uri, uri=True)
assert c.execute("SELECT height, pending, (SELECT count(*) FROM notes) FROM hpplite_status").fetchone() == (h + 2, 0, 290)
c.close()
print("python smoke: ok")
# a connection left to the interpreter's exit: some Pythons clear its
# callbacks as they close it then, which is not another component taking
# them over (smoke.sh fails on the warning; test_loadable covers it in C)
left = sqlite3.connect(uri, uri=True)
left.execute("INSERT INTO notes(body, n) VALUES('left open', 0)")
left.commit()
