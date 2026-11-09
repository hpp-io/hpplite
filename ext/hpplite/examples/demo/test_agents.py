#!/usr/bin/env python3
"""The demo's agents reach their model through an OpenAI-compatible endpoint with what a gateway needs (#262):
a User-Agent (Cloudflare answers Python's default one 403), the key as a Bearer token, and room for a reasoning
model (its reasoning counts against max_tokens: 90 gave an empty post; reasoning_effort low, 300 tokens). The URL,
model and key come from the environment or else ext/hpplite/.env. A stub server stands in for the endpoint.
    python3 examples/demo/test_agents.py
"""
import http.server, json, os, sys, tempfile, threading

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, here)

seen = []
class Stub(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers["content-length"])))
        seen.append((self.path, {k.lower(): v for k, v in self.headers.items()}, body))
        text = "" if body.get("model") == "stub/empty" else "hello from the stub"
        out = json.dumps({"choices": [{"message": {"role": "assistant", "content": text}, "finish_reason": "stop"}],
                          "usage": {"prompt_tokens": 1, "completion_tokens": 1}}).encode()
        self.send_response(200); self.send_header("content-type", "application/json"); self.send_header("content-length", str(len(out)))
        self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass

srv = http.server.HTTPServer(("127.0.0.1", 0), Stub)
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]

def fail(msg):
    print("test_agents: FAIL:", msg); sys.exit(1)

# 1. the environment names the endpoint, the model and the key; the request carries what the gateway needs
os.environ["HPPLITE_DEMO_LLM_URL"] = f"http://127.0.0.1:{port}/v1"
os.environ["HPPLITE_DEMO_MODEL"] = "stub/model"
os.environ["HPPLITE_DEMO_LLM_KEY"] = "test-key-1"
import agents
status = {"batches": 412, "pending": 3}
recent = [{"name": "Krillian", "body": "BATCH 412!!! HISTORIC!!!"}]
text = agents._rpc_local(agents.AGENTS[1], status, recent, None)
text == "hello from the stub" or fail(f"the stub's answer did not come back as the post: {text!r}")
path, headers, body = seen[-1]
path == "/v1/chat/completions" or fail(f"posted to {path}")
headers.get("authorization") == "Bearer test-key-1" or fail(f"no Bearer key: {headers.get('authorization')!r}")
headers.get("user-agent", "").startswith("hpplite-demo") or fail(f"no User-Agent of the demo's own: {headers.get('user-agent')!r}")
body.get("model") == "stub/model" or fail(f"model {body.get('model')!r}")
body.get("max_tokens", 0) >= 300 or fail(f"max_tokens {body.get('max_tokens')}: a reasoning model's reasoning counts against it")
body.get("reasoning_effort") == "low" or fail(f"reasoning_effort {body.get('reasoning_effort')!r}")

# 2. an empty answer (the reasoning ate the budget) is no post: the writer falls through to the next backend
agents.LOCAL_MODEL = "stub/empty"
agents._rpc_local(agents.AGENTS[2], status, recent, None) is None or fail("an empty answer was taken as a post")
agents.LOCAL_MODEL = "stub/model"

# 3. without the environment, ext/hpplite/.env supplies the three settings
for k in ("HPPLITE_DEMO_LLM_URL", "HPPLITE_DEMO_MODEL", "HPPLITE_DEMO_LLM_KEY"): del os.environ[k]
with tempfile.NamedTemporaryFile("w", suffix=".env", delete=False) as f:
    f.write("# the demo's keys\nHPPLITE_PRIVATE_KEY=0xabc\nHPPLITE_DEMO_LLM_KEY=\"from-dotenv\"\nHPPLITE_DEMO_MODEL=dotenv/model\n")
    envfile = f.name
try:
    agents._ENV_FILE = envfile; agents._dotenv_cache = None
    agents._setting("HPPLITE_DEMO_LLM_KEY") == "from-dotenv" or fail("the key in .env is not read (quotes stripped)")
    agents._setting("HPPLITE_DEMO_MODEL", "x") == "dotenv/model" or fail("the model in .env is not read")
    agents._setting("HPPLITE_DEMO_LLM_URL", "https://router.hpp.io/llm/v1") == "https://router.hpp.io/llm/v1" or fail("a setting in neither place is not its default")
    os.environ["HPPLITE_DEMO_MODEL"] = "env/model"
    agents._setting("HPPLITE_DEMO_MODEL", "x") == "env/model" or fail("the environment does not win over .env")
    agents._ENV_FILE = "/nonexistent/.env"; agents._dotenv_cache = None
    agents._setting("HPPLITE_DEMO_LLM_KEY") is None or fail("a missing .env is not an empty one")
finally:
    os.unlink(envfile)

# 4. the writer's name says the model and where it is
agents._local_down_until = 0
"stub/model" in agents.writer_name() or fail(f"writer_name does not name the model: {agents.writer_name()!r}")
print("test_agents: the request carries the Bearer key, a User-Agent, 300 tokens and low reasoning effort; an empty answer is no post; .env supplies the settings under the environment")
