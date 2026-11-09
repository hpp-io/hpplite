#!/usr/bin/env python3
"""rpc_stub.py over TLS, with a store's /status beside it (#269): what HPPLite's two curl clients - the chain
RPC (eth/eth_client.c) and the store (store.c) - reach when HPPLITE_CA_BUNDLE must name the certificate that
signed this server. Test infrastructure; nothing is sent anywhere.

    tls_stub.py --port 8545 --cert cert.pem --key key.pem
"""
import argparse
import json
import os
import ssl
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))     # python -I leaves the script's directory out
import rpc_stub


class Handler(rpc_stub.Handler):
    def do_GET(self):
        if self.path != "/status":
            self.send_response(404)
            self.end_headers()
            return
        body = json.dumps({"service": "hpplite-store", "protocol": "hpplite-store/2", "signer": "0x" + "11" * 20}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--cert", required=True)
    ap.add_argument("--key", required=True)
    a = ap.parse_args()
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(a.cert, a.key)
    srv = rpc_stub.Server(("127.0.0.1", a.port), Handler)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
