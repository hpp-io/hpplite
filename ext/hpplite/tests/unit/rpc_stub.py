#!/usr/bin/env python3
"""A JSON-RPC stand-in for a chain whose coordinator reports a given VERSION (#252): enough of eth_* for
hpplite_l1_connect and getSystemConfig(). Test infrastructure; nothing is sent anywhere.

    rpc_stub.py --port 8545 --version 7
"""
import argparse
import http.server
import json
import socketserver

GET_SYSTEM_CONFIG = "0x560f73f8"      # keccak256("getSystemConfig()")[:4]
GET_STATE = "0x1865c57d"              # keccak256("getState()")[:4]


def word(n):
    return "%064x" % n


def system_config(version):
    da = word(0x2222222222222222222222222222222222222222)
    scheme = b"hppda"
    return "0x" + word(0xc0) + da + word(version) + word(31337) + word(100) + word(300) \
        + word(len(scheme)) + scheme.hex().ljust(64, "0")


def state():
    return "0x" + word(0x1111111111111111111111111111111111111111) * 2 + word(0) * 5


class Handler(http.server.BaseHTTPRequestHandler):
    version = 10

    def log_message(self, fmt, *args):
        pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0)
        try:
            req = json.loads(self.rfile.read(n) if n else b"{}")
        except ValueError:
            req = {}
        reqs = req if isinstance(req, list) else [req]
        out = [self.answer(r) for r in reqs]
        body = json.dumps(out if isinstance(req, list) else out[0]).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def answer(self, r):
        m = r.get("method")
        p = r.get("params") or []
        if m == "eth_chainId":
            result = "0x7a69"
        elif m in ("eth_blockNumber", "eth_gasPrice", "eth_getTransactionCount", "eth_estimateGas"):
            result = "0x10"
        elif m == "eth_getCode":
            result = "0x6060"
        elif m == "eth_getBalance":
            result = "0x" + word(10**18)
        elif m == "eth_call":
            data = (p[0].get("data") or "") if p and isinstance(p[0], dict) else ""
            if data.startswith(GET_SYSTEM_CONFIG):
                result = system_config(self.version)
            elif data.startswith(GET_STATE):
                result = state()
            else:
                result = "0x" + word(0)
        elif m == "eth_getLogs":
            result = []
        else:
            return {"jsonrpc": "2.0", "id": r.get("id"), "error": {"code": -32601, "message": "method not found: %s" % m}}
        return {"jsonrpc": "2.0", "id": r.get("id"), "result": result}


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--version", type=int, default=10)
    args = ap.parse_args()
    Handler.version = args.version
    srv = Server(("127.0.0.1", args.port), Handler)
    print("rpc_stub: listening on 127.0.0.1:%d, VERSION %d" % (args.port, args.version), flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
