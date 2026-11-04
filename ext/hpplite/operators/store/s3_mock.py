#!/usr/bin/env python3
"""
An S3 stand-in for tests: PUT and GET of one bucket's objects, with the
request's Signature Version 4 checked against the shared secret the way S3
checks it. Standard library only. Objects live in memory.

    s3_mock.py --port 9000 --access-key AKIATEST --secret-key secret [--region us-east-1]

  PUT  /<bucket>/<key>   stores the body (x-amz-content-sha256 must be its sha-256); 200 with an ETag
  GET  /<bucket>/<key>   the bytes, or 404 NoSuchKey
  A bad or missing signature is 403 SignatureDoesNotMatch; S3's XML error shapes.
"""
import argparse
import hashlib
import hmac
import http.server
import socketserver
import urllib.parse

OBJECTS = {}
ARGS = None


def _hmac(key, msg):
    return hmac.new(key, msg.encode() if isinstance(msg, str) else msg, hashlib.sha256).digest()


def signing_key(secret, date, region, service):
    k = _hmac(("AWS4" + secret).encode(), date)
    k = _hmac(k, region)
    k = _hmac(k, service)
    return _hmac(k, "aws4_request")


def canonical_uri(path):
    return "/".join(urllib.parse.quote(seg, safe="-_.~") for seg in path.split("/"))


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _error(self, code, name, message):
        body = ('<?xml version="1.0" encoding="UTF-8"?>\n<Error><Code>%s</Code><Message>%s</Message></Error>' % (name, message)).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/xml")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _verify(self, body):
        """Recompute the signature of this request; None when it matches, else why not."""
        auth = self.headers.get("Authorization", "")
        if not auth.startswith("AWS4-HMAC-SHA256 "):
            return "no AWS4-HMAC-SHA256 authorization"
        parts = dict(p.strip().split("=", 1) for p in auth[len("AWS4-HMAC-SHA256 "):].split(","))
        cred = parts.get("Credential", "").split("/")
        if len(cred) != 5 or cred[0] != ARGS.access_key:
            return "unknown access key"
        date, region, service = cred[1], cred[2], cred[3]
        if region != ARGS.region or service != "s3":
            return "scope is not this region's s3"
        signed = parts.get("SignedHeaders", "").split(";")
        amz_date = self.headers.get("x-amz-date", "")
        payload_hash = self.headers.get("x-amz-content-sha256", "")
        if payload_hash != hashlib.sha256(body).hexdigest():
            return "x-amz-content-sha256 is not the body's"
        url = urllib.parse.urlsplit(self.path)
        query = "&".join(sorted("%s=%s" % (urllib.parse.quote(k, safe="-_.~"), urllib.parse.quote(v, safe="-_.~"))
                                for k, v in urllib.parse.parse_qsl(url.query, keep_blank_values=True)))
        canonical_headers = "".join("%s:%s\n" % (h, " ".join(self.headers.get(h, "").split())) for h in signed)
        canonical = "\n".join([self.command, canonical_uri(url.path), query, canonical_headers, ";".join(signed), payload_hash])
        scope = "/".join([date, region, service, "aws4_request"])
        to_sign = "\n".join(["AWS4-HMAC-SHA256", amz_date, scope, hashlib.sha256(canonical.encode()).hexdigest()])
        want = hmac.new(signing_key(ARGS.secret_key, date, region, service), to_sign.encode(), hashlib.sha256).hexdigest()
        if want != parts.get("Signature", ""):
            return "The request signature we calculated does not match the signature you provided."
        return None

    def _key(self):
        path = urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)
        parts = path.lstrip("/").split("/", 1)
        if len(parts) != 2 or parts[0] != ARGS.bucket:
            return None
        return parts[1]

    def do_PUT(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else b""
        why = self._verify(body)
        if why:
            return self._error(403, "SignatureDoesNotMatch", why)
        key = self._key()
        if key is None:
            return self._error(404, "NoSuchBucket", "The specified bucket does not exist")
        OBJECTS[key] = body
        self.send_response(200)
        self.send_header("ETag", '"%s"' % hashlib.md5(body).hexdigest())
        self.send_header("Content-Length", "0")
        self.end_headers()

    def do_GET(self):
        why = self._verify(b"")
        if why:
            return self._error(403, "SignatureDoesNotMatch", why)
        key = self._key()
        data = OBJECTS.get(key) if key is not None else None
        if data is None:
            return self._error(404, "NoSuchKey", "The specified key does not exist.")
        self.send_response(200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=9000)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--bucket", default="batches")
    ap.add_argument("--access-key", required=True)
    ap.add_argument("--secret-key", required=True)
    ap.add_argument("--region", default="us-east-1")
    ARGS = ap.parse_args()
    Server((ARGS.bind, ARGS.port), Handler).serve_forever()


if __name__ == "__main__":
    main()
