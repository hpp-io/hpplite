"""HPPLite demo: a public timeline whose every write lands on HPP Sepolia.

    HPPLITE_PRIVATE_KEY=0x... python3 app.py /path/to/hpplite.dylib [port]

Stock Python sqlite3 + the loadable module. One hpplite=on connection (the
sequencer) behind a lock; a cast of resident AI agents (agents.py, written by
a local model through LM Studio/Ollama, else Claude, else templates) posts
every 45-90 s so the ledger moves even when nobody is watching; /api/verify
rebuilds the database from the chain alone in a separate process and reports.
"""
import json, os, random, sqlite3, subprocess, sys, threading, time, urllib.request
import agents
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse, parse_qs

EXT = sys.argv[1]
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8787
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")
RPC = "https://sepolia.hpp.io"
EXPLORER = "https://sepolia-explorer.hpp.io"
SEL_GET_BATCH = "5ac44282"            # cast sig "getBatch(uint256)"
STORE = os.environ.get("HPPLITE_DEMO_STORE")   # run_demo.sh starts a pin service: the bytes there, one anchor per 30 min (#164)
URI = f"file:{DATA}/timeline.db?hpplite=on&l1=hpp-sepolia&interval=90s&checkpoint=5&datadir={DATA}" + (f"&store={STORE}&anchor=30m" if STORE else "")
MAX_ENTRIES = 50000                   # ~100 B each: nowhere near the v0.1 100 MB bound

os.makedirs(DATA, exist_ok=True)
boot = sqlite3.connect(":memory:")
boot.enable_load_extension(True); boot.load_extension(os.path.splitext(EXT)[0]); boot.close()

db = sqlite3.connect(URI, uri=True, check_same_thread=False, isolation_level=None)
lock = threading.Lock()

def bookkeeping():
    """Where HPPLite keeps its tables: the file itself (row capture), or the sidecar beside it (page capture, the
    default for a new rollup, #146). The sidecar is the node's: a short busy timeout, never a wait on it."""
    try:
        db.execute("SELECT 1 FROM _hpplite_meta").fetchone()
        return db
    except sqlite3.OperationalError:
        return sqlite3.connect(f"file:{DATA}/timeline.db-hpplite.db?mode=ro", uri=True, check_same_thread=False, timeout=0.2)
with lock:
    db.execute("CREATE TABLE IF NOT EXISTS entries(id INTEGER PRIMARY KEY, name TEXT NOT NULL, body TEXT NOT NULL, posted_at INTEGER NOT NULL, ip_hash TEXT)")
    cols = [r[1] for r in db.execute("PRAGMA table_info(entries)")]
    if "kind" not in cols:
        db.execute("ALTER TABLE entries ADD COLUMN kind TEXT NOT NULL DEFAULT 'human'")   # captured and replayed like any DDL
    if "ip_hash" in cols:
        # a derived identifier has no business in a permanent (if encrypted) log; rate limiting is in memory
        db.execute("ALTER TABLE entries DROP COLUMN ip_hash")
    st = db.execute("SELECT height, pending, contract FROM hpplite_status").fetchone()
    print(f"hpplite: height {st[0]}, pending {st[1]}, contract {st[2]}", flush=True)

def status():
    with lock:
        r = db.execute("SELECT height, pending, state_root, last_checkpoint_height, lease, log_errors, "
                       "replay_skipped, encryption, keyed, nondeterministic, contract, da_contract, last_batch_tx, "
                       "anchored, last_anchor_tx, store, store_state, store_calls, last_checkpoint_root "
                       "FROM hpplite_status").fetchone()
        n = db.execute("SELECT count(*) FROM entries").fetchone()[0]
        size = db.execute("SELECT page_count * page_size FROM pragma_page_count(), pragma_page_size()").fetchone()[0]
    keys = ["height", "pending", "state_root", "last_checkpoint_height", "lease", "log_errors",
            "replay_skipped", "encryption", "keyed", "nondeterministic", "contract", "da_contract", "last_batch_tx",
            "anchored", "last_anchor_tx", "store", "store_state", "store_calls", "last_checkpoint_root"]
    d = dict(zip(keys, r)); d["entries"] = n; d["db_bytes"] = size; d["explorer"] = EXPLORER
    d["anchored_root"] = d["last_checkpoint_root"]
    d["batches"] = d["height"] - 1
    d["agents"] = [dict(name=a["name"], emoji=a["emoji"]) for a in agents.AGENTS]
    d["writer"] = agents.writer_name()
    return d

def entries(after=0, limit=50):
    with lock:
        rows = db.execute("SELECT id, name, body, posted_at, kind FROM entries WHERE id > ? ORDER BY id DESC LIMIT ?", (after, limit)).fetchall()
    return [dict(id=r[0], name=r[1], body=r[2], posted_at=r[3], kind=r[4]) for r in rows]

def add_entry(name, body, ip, kind="human"):
    name = (name or "anonymous").strip()[:40]
    body = (body or "").strip()[:280]
    if not body: return None
    if kind == "human" and name in agents.NAMES: name += " (human)"
    with lock:
        if db.execute("SELECT count(*) FROM entries").fetchone()[0] >= MAX_ENTRIES: return None
        # the timestamp is bound by the application: replay sees the literal, never 'now'
        cur = db.execute("INSERT INTO entries(name, body, posted_at, kind) VALUES(?,?,?,?)", (name, body, int(time.time()), kind))
        return cur.lastrowid

def schema():
    """Every table in the file - the app's and HPPLite's own bookkeeping - with a short preview."""
    out = []
    with lock:
        for name, sql in db.execute("SELECT name, sql FROM sqlite_master WHERE type='table' ORDER BY name LIKE '\\_hpplite\\_%' ESCAPE '\\', name"):
            cols = [r[1] for r in db.execute(f'PRAGMA table_info("{name}")')]
            n = db.execute(f'SELECT count(*) FROM "{name}"').fetchone()[0]
            shown = [c for c in cols if c != "ip_hash"]
            rows = db.execute(f'SELECT {",".join(chr(34)+c+chr(34) for c in shown)} FROM "{name}" ORDER BY rowid DESC LIMIT 3').fetchall()
            def cell(v):
                if isinstance(v, bytes): return "0x" + v.hex()[:16] + "…" if len(v) > 8 else "0x" + v.hex()
                s = str(v) if v is not None else "NULL"
                return s if len(s) <= 60 else s[:57] + "…"
            out.append(dict(name=name, sql=sql, columns=shown, rows=n,
                            note={"_hpplite_pending": "the write log: SQL text captured inside your transaction, waiting for the next batch",
                                  "_hpplite_meta": "next batch height, previous block hash, state_root = root as of the last root-carrying batch (updated at checkpoint cadence, not per write; the live root is on the card above), last checkpoint - the only HPPLite state there is"}.get(name, ""),
                            preview=[[cell(v) for v in r] for r in rows]))
    return out

def describe(height, raw, where):
    length = len(raw)
    last_batch_bytes["n"] = length
    return {"height": height, "bytes": length, "magic": raw[:4].decode("latin1"), "where": where,
            "scheme_id": raw[4] if length > 4 else None, "hex_head": raw[:48].hex(), "hex_tail": raw[-16:].hex(),
            "sealed": raw.startswith(b"HPE")}

def chain_batch(height):
    """What holds one batch: the store (#164: by the reference the node kept until the anchor), else eth_call HPPLiteDA.getBatch(height)."""
    s = status()
    if not s["da_contract"] or height < 1 or height >= s["height"]: return {"height": height, "error": "no such batch yet"}
    if STORE:
        with lock:
            try:
                row = bookkeeping().execute("SELECT ref FROM _hpplite_unanchored WHERE height = ?", (height,)).fetchone()
            except Exception:
                row = None
        if row:
            with urllib.request.urlopen(f"{STORE}/get/{row[0]}", timeout=20) as r:
                return describe(height, r.read(), f"the store, {row[0][:16]}…, waiting for the anchor")
        return {"height": height, "error": f"batch {height} is at the store, anchored on the chain in a range (bytes shown while they wait for the anchor)"}
    data = "0x" + SEL_GET_BATCH + format(height, "064x")
    req = urllib.request.Request(RPC, data=json.dumps({"jsonrpc": "2.0", "id": 1, "method": "eth_call",
                                 "params": [{"to": s["da_contract"], "data": data}, "latest"]}).encode(),
                                 headers={"content-type": "application/json", "user-agent": "hpplite-demo/0.1"})
    with urllib.request.urlopen(req, timeout=20) as r:
        res = json.load(r)["result"][2:]
    length = int(res[64:128], 16)
    raw = bytes.fromhex(res[128:128 + 2 * length])
    return describe(height, raw, "the chain")

# the resident agents: one posts every 45-90 s, so there is always something to batch
last_batch_bytes = {"n": 0}
def heartbeat():
    rng = random.Random()
    time.sleep(20)
    while True:
        try:
            s = status(); s["last_batch_bytes"] = last_batch_bytes["n"]
            agent, text = agents.next_post(s, entries(0, 12), rng)
            add_entry(agent["name"], text, "agent:" + agent["name"], kind="agent")
        except Exception as e:
            print("agents:", e, flush=True)
        time.sleep(rng.uniform(30, 75))      # ~2 posts per 90 s batch: batching actually batches
threading.Thread(target=heartbeat, daemon=True).start()

last_post = {}
verify_lock = threading.Lock()
last_verify = {"t": 0.0}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def send_json(self, obj, code=200):
        b = json.dumps(obj).encode()
        self.send_response(code); self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(b)))
        self.send_header("cache-control", "no-store"); self.end_headers(); self.wfile.write(b)
    def client_ip(self):
        return self.headers.get("CF-Connecting-IP") or self.headers.get("X-Forwarded-For", "").split(",")[0].strip() or self.client_address[0]
    def do_GET(self):
        u = urlparse(self.path); q = parse_qs(u.query)
        try:
            if u.path == "/":
                b = open(os.path.join(HERE, "index.html"), "rb").read()
                self.send_response(200); self.send_header("content-type", "text/html; charset=utf-8"); self.send_header("content-length", str(len(b))); self.end_headers(); self.wfile.write(b)
            elif u.path == "/api/status": self.send_json(status())
            elif u.path == "/api/entries": self.send_json(entries(int(q.get("after", ["0"])[0])))
            elif u.path == "/api/batch": self.send_json(chain_batch(int(q.get("h", ["0"])[0])))
            elif u.path == "/api/schema": self.send_json(schema())
            else: self.send_json({"error": "not found"}, 404)
        except Exception as e:
            self.send_json({"error": str(e)}, 500)
    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("content-length", "0") or 0)
        body = self.rfile.read(min(n, 4096)) if n else b""
        ip = self.client_ip()
        try:
            if u.path == "/api/entries":
                now = time.time()
                if now - last_post.get(ip, 0) < 3: return self.send_json({"error": "one entry every 3 s per visitor"}, 429)
                j = json.loads(body or b"{}")
                rid = add_entry(j.get("name"), j.get("body"), ip)
                if rid is None: return self.send_json({"error": "empty entry or timeline full"}, 400)
                last_post[ip] = now
                self.send_json({"id": rid, "status": status()})
            elif u.path == "/api/verify":
                if time.time() - last_verify["t"] < 20: return self.send_json({"error": "verify ran less than 20 s ago; try again shortly"}, 429)
                if not verify_lock.acquire(blocking=False): return self.send_json({"error": "a verification is already running"}, 429)
                last_verify["t"] = time.time()
                try:
                    p = subprocess.run([sys.executable, os.path.join(HERE, "verify_rebuild.py"), EXT], capture_output=True, text=True, timeout=900,
                                       env={**os.environ})
                    out = json.loads(p.stdout.strip().splitlines()[-1]) if p.stdout.strip() else {"ok": False, "error": p.stderr[-1000:]}
                    out["live"] = status()
                    self.send_json(out)
                finally:
                    verify_lock.release()
            else: self.send_json({"error": "not found"}, 404)
        except Exception as e:
            self.send_json({"error": str(e)}, 500)

print(f"serving on http://localhost:{PORT}", flush=True)
srv = ThreadingHTTPServer(("127.0.0.1", PORT), H)
import signal
signal.signal(signal.SIGTERM, lambda *a: threading.Thread(target=srv.shutdown, daemon=True).start())
try:
    srv.serve_forever()
finally:
    with lock:
        db.close()          # flushes what is pending and releases the lease (a kill -9 would leave it for 5 min)
    print("closed", flush=True)
