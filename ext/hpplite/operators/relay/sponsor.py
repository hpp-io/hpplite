#!/usr/bin/env python3
"""A funder's side of a relay's accounts and sponsorship (#276, doc/relay-protocol.md): what a platform's control
plane does so its apps' keys need no money at the relay. Standard library plus ../x402seller.

    sponsor.py <relay-url> --key-file FILE permit <value> [--days 365]   the funder's EIP-2612 permit at the relay
    sponsor.py <relay-url> --key-file FILE [--rollup-key-file FILE] sponsor <rollup>
                                                                         the funder pays for this rollup's calls; the
                                                                         rollup's owner or sequencer signs beside it
                                                                         unless the funder is one of them
    sponsor.py <relay-url> --key-file FILE cap <rollup> <callsPerDay>    bound the rollup's day (0: no bound)
    sponsor.py <relay-url> --key-file FILE release <rollup>              stop paying for it
    sponsor.py <relay-url> --key-file FILE account                        the funder's account as the relay sees it
    sponsor.py <relay-url> sponsor-of <rollup>                            who pays for a rollup

Keys never go on the command line (--key-file or HPPLITE_FUNDER_KEY; --rollup-key-file or HPPLITE_ROLLUP_KEY). Prints
the relay's JSON answer; exits 0 on a 2xx."""
import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
from x402seller import eth  # noqa: E402

SPONSOR_TYPE = "Sponsor(address rollup,address funder,uint256 issuedAt)"
UNSPONSOR_TYPE = "Unsponsor(address rollup,address funder,uint256 issuedAt)"
CAP_TYPE = "Cap(address rollup,uint256 callsPerDay,uint256 issuedAt)"
PERMIT_TYPE = "Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)"


def http(method, url, body=None):
    req = urllib.request.Request(url, data=json.dumps(body).encode() if body is not None else None, method=method,
                                 headers={"Content-Type": "application/json"} if body is not None else {})
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, json.loads(r.read() or b"{}")
    except urllib.error.HTTPError as e:
        try:
            return e.code, json.loads(e.read() or b"{}")
        except ValueError:
            return e.code, {}


def main():
    ap = argparse.ArgumentParser(description="a funder at an HPPLite relay (#276)")
    ap.add_argument("relay")
    ap.add_argument("--key-file", default=None, help="the funder's hex key; or HPPLITE_FUNDER_KEY in the environment")
    ap.add_argument("--rollup-key-file", default=None, help="the rollup's owner or sequencer key, signing a sponsorship beside "
                    "the funder when the funder is neither; or HPPLITE_ROLLUP_KEY in the environment")
    ap.add_argument("--days", type=int, default=365, help="a permit's life")
    ap.add_argument("action", choices=["permit", "sponsor", "cap", "release", "account", "sponsor-of"])
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    url = a.relay.rstrip("/")
    st, status = http("GET", url + "/status")
    if st != 200 or "accounts" not in status:
        print(json.dumps({"error": "not a relay with accounts (%s)" % st, "status": status}))
        return 2
    acc = status["accounts"]
    if a.action == "sponsor-of":
        st, out = http("GET", url + "/sponsor/" + a.args[0].lower())
        print(json.dumps(out))
        return 0 if st == 200 else 1
    key_hex = open(a.key_file).read().strip() if a.key_file else os.environ.get("HPPLITE_FUNDER_KEY", "")
    key = bytes.fromhex(key_hex[2:] if key_hex.startswith("0x") else key_hex)
    funder = eth.address(key).lower()
    now = int(time.time())
    if a.action == "account":
        st, out = http("GET", url + "/account/" + funder)
        print(json.dumps(out))
        return 0 if st == 200 else 1
    if a.action == "permit":
        value, deadline = int(a.args[0]), now + a.days * 86400
        st, view = http("GET", url + "/account/" + funder)
        nonce = int((view or {}).get("permitNonce") or 0) if st == 200 else 0
        d = eth.domain_separator(acc["assetDomain"]["name"], acc["assetDomain"]["version"], int(status["chainId"]), acc["asset"])
        sig = eth.sign(key, eth.typed_digest(d, PERMIT_TYPE, funder, acc["spender"], value, nonce, deadline))
        st, out = http("POST", url + "/account/%s/permit" % funder, {"value": value, "deadline": deadline, "signature": "0x" + sig.hex()})
    else:
        rollup = a.args[0].lower()
        dom = acc["domain"]
        d = eth.domain_separator(dom["name"], dom["version"], int(dom["chainId"]), dom["verifyingContract"])
        if a.action == "cap":
            calls = int(a.args[1])
            sig = eth.sign(key, eth.typed_digest(d, CAP_TYPE, rollup, calls, now))
            st, out = http("POST", url + "/sponsor/%s/cap" % rollup, {"funder": funder, "callsPerDay": calls, "issuedAt": now, "signature": "0x" + sig.hex()})
        else:
            typ, path = (UNSPONSOR_TYPE, "/release") if a.action == "release" else (SPONSOR_TYPE, "")
            digest = eth.typed_digest(d, typ, rollup, funder, now)
            body = {"funder": funder, "issuedAt": now, "signature": "0x" + eth.sign(key, digest).hex()}
            rkey_hex = open(a.rollup_key_file).read().strip() if a.rollup_key_file else os.environ.get("HPPLITE_ROLLUP_KEY", "")
            if a.action == "sponsor" and rkey_hex:
                rkey = bytes.fromhex(rkey_hex[2:] if rkey_hex.startswith("0x") else rkey_hex)
                body["rollupSignature"] = "0x" + eth.sign(rkey, digest).hex()
            st, out = http("POST", url + "/sponsor/%s%s" % (rollup, path), body)
    print(json.dumps(out))
    return 0 if 200 <= st < 300 else 1


if __name__ == "__main__":
    sys.exit(main())
