#!/usr/bin/env python3
"""
A Kubo stand-in for tests: the two RPC calls HPPLite's kubo:// store makes,
with Kubo's shapes, and nothing else. Standard library only.

    kubo_mock.py --port 5001

  POST /api/v0/add?cid-version=1&raw-leaves=true&pin=true   multipart "file" -> {"Name","Hash","Size"} (one JSON line)
  POST /api/v0/cat?arg=<cid>                                  the bytes, or 500 {"Message","Code","Type"} as Kubo answers
  GET  anything                                               405, as Kubo refuses GET on its RPC

The CID is CIDv1 raw sha2-256 in base32, what Kubo computes for a single
raw leaf (files under its chunk size). Bytes live in memory.
"""
import argparse
import hashlib
import http.server
import json
import socketserver
import urllib.parse


def _base32_lower(b: bytes) -> str:
    alphabet = "abcdefghijklmnopqrstuvwxyz234567"
    bits = "".join(format(x, "08b") for x in b)
    bits += "0" * (-len(bits) % 5)
    return "".join(alphabet[int(bits[i:i + 5], 2)] for i in range(0, len(bits), 5))


def cid_of(data: bytes) -> str:
    mh = b"\x12\x20" + hashlib.sha256(data).digest()
    return "b" + _base32_lower(b"\x01\x55" + mh)


BLOCKS = {}


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _json(self, code, obj):
        body = (json.dumps(obj) + "\n").encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self._json(405, {"Message": "Method Not Allowed", "Code": 0, "Type": "error"})

    def do_POST(self):
        url = urllib.parse.urlsplit(self.path)
        query = urllib.parse.parse_qs(url.query)
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        if url.path == "/api/v0/add":
            ctype = self.headers.get("Content-Type", "")
            if "boundary=" not in ctype:
                return self._json(400, {"Message": "file argument 'path' is required", "Code": 1, "Type": "error"})
            boundary = ("--" + ctype.split("boundary=", 1)[1].strip()).encode()
            # the first part's payload: after its blank line, before the next boundary
            start = body.find(boundary)
            head_end = body.find(b"\r\n\r\n", start)
            if start < 0 or head_end < 0:
                return self._json(400, {"Message": "malformed multipart", "Code": 1, "Type": "error"})
            data_start = head_end + 4
            data_end = body.find(b"\r\n" + boundary, data_start)
            data = body[data_start:data_end if data_end >= 0 else len(body)]
            if query.get("cid-version", ["0"])[0] != "1" or query.get("raw-leaves", ["false"])[0] != "true":
                return self._json(400, {"Message": "this stand-in only knows cid-version=1&raw-leaves=true", "Code": 1, "Type": "error"})
            cid = cid_of(data)
            BLOCKS[cid] = data
            return self._json(200, {"Name": "batch", "Hash": cid, "Size": str(len(data) + 8)})
        if url.path == "/api/v0/cat":
            cid = query.get("arg", [""])[0]
            data = BLOCKS.get(cid)
            if data is None:
                return self._json(500, {"Message": "ipld: could not find %s" % cid, "Code": 0, "Type": "error"})
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)
            return
        self._json(404, {"Message": "404 page not found", "Code": 0, "Type": "error"})


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=5001)
    ap.add_argument("--bind", default="127.0.0.1")
    args = ap.parse_args()
    Server((args.bind, args.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
