"""Rebuild the timeline database from HPP Sepolia alone and report.

Opens an empty file with the same key and l1=hpp-sepolia: HPPLite fetches
every batch, opens it, replays it, and compares each posted state root with
the content hash it computes ("state root mismatch" on stderr when they
differ). Prints one JSON object. Used by app.py's Verify button; also a
fine thing to run by hand.
"""
import json, os, sqlite3, subprocess, sys, tempfile, time

ext = sys.argv[1]
key = os.environ["HPPLITE_PRIVATE_KEY"]

def rebuild():
    d = tempfile.mkdtemp(prefix="hpplite-verify-")
    store = os.environ.get("HPPLITE_STORE", "on")
    uri = f"file:{d}/rebuilt.db?hpplite=on&l1=hpp-sepolia&interval=0&dbname=timeline-v07&datadir={d}" + (f"&store={store}" if store != "off" else "")
    t0 = time.time()
    c = sqlite3.connect(":memory:")
    c.enable_load_extension(True); c.load_extension(os.path.splitext(ext)[0]); c.close()
    c = sqlite3.connect(uri, uri=True)
    st = c.execute("SELECT height, pending, state_root, last_checkpoint_height, replay_skipped, contract, "
                   "roots_compared, root_mismatches, foreign_batches FROM hpplite_status").fetchone()
    tables = [r[0] for r in c.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE '\\_hpplite\\_%' ESCAPE '\\'")]
    rows = c.execute("SELECT count(*) FROM entries").fetchone()[0] if "entries" in tables else 0
    latest = c.execute("SELECT name, body FROM entries ORDER BY id DESC LIMIT 1").fetchone() if rows else None
    c.close()
    for f in os.listdir(d):
        os.unlink(os.path.join(d, f))
    os.rmdir(d)
    return {"height": st[0], "batches": st[0] - 1, "rows": rows, "state_root": st[2],
            "last_checkpoint_height": st[3], "replay_skipped": st[4], "contract": st[5],
            "roots_compared": st[6], "root_mismatches": st[7], "foreign_batches": st[8],
            "latest": latest, "seconds": round(time.time() - t0, 1)}

if len(sys.argv) > 2 and sys.argv[2] == "--child":
    print(json.dumps(rebuild()))
    sys.exit(0)

# run the rebuild in a child so its stderr (the replay's own root comparison) can be counted
p = subprocess.run([sys.executable, __file__, ext, "--child"], capture_output=True, text=True, timeout=600)
err = p.stderr
out = {"ok": p.returncode == 0}
try:
    out.update(json.loads(p.stdout.strip().splitlines()[-1]))
except Exception:
    out["error"] = (p.stdout + err)[-2000:]
out.setdefault("root_mismatches", err.count("state root mismatch"))   # the library counts; stderr is the fallback
out.setdefault("roots_compared", 0)
out["stderr"] = [l for l in err.splitlines() if "mismatch" in l or "cannot" in l or "failed" in l][:20]
print(json.dumps(out))
