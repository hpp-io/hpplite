"""A store's side of hpplite-fleet/1 (#297, doc/fleet-protocol.md, "The store"): the fleet's state from its events,
the name write that carries a database's signed head (checked against the database's sequencer, lease and epoch -
the epoch is the fence), the heads an anchor builds the next root from, and the latest root per database from
RootPosted with the manifest checked against digest and root.

Money for fleet databases (BindDb and its siblings) is not here yet: a store that charges answers fleet writes 501.
"""
import json
import os
import sys
import threading

here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(here, ".."))
sys.path.insert(0, os.path.join(here, "..", "fleet"))
from x402seller import eth  # noqa: E402
from x402seller.chain import RpcError, selector  # noqa: E402
from x402seller.token import ChainUnreadable  # noqa: E402
import fleet as F  # noqa: E402

EVENTS = {eth.keccak256(s.encode()).hex(): s.split("(")[0] for s in (
    "Admitted(bytes32,address,address,bytes32,address)", "Claimed(bytes32,bytes32,uint64)", "EpochMoved(bytes32,uint64,uint64)",
    "RootPosted(uint256,bytes32,bytes32)", "Retired(bytes32)", "Promoted(bytes32,address)", "SequencerSet(bytes32,address)")}
HEAD_FIELDS = ("dbId", "instanceId", "fromHeight", "toHeight", "chainHashAfter", "stateRoot", "manifestDigest", "epoch")
ZERO = bytes(32)


def chain_hash_next(prev, height, batch_keccak):
    """the DA contract's rolling hash: keccak256(abi.encode(prev, height, batchHash)) (finalize.c)"""
    return eth.keccak256(prev + height.to_bytes(32, "big") + batch_keccak)


def _w(data, i):
    return data[64 * i:64 * (i + 1)]


class FleetBook:
    """Per fleet: every database's sequencer, owner, lease instance, epoch and retirement, from the fleet's logs;
    the roots posted and which of them checked; per database the latest head held and the latest indexed root"""

    def __init__(self, chain, disk, chain_id):
        self.chain, self.disk, self.chain_id = chain, disk, chain_id
        self.lock = threading.RLock()

    def _key(self, fleet):
        return "f/%s/state.json" % fleet

    def state(self, fleet):
        return self.disk.json(self._key(fleet)) or {"fleet": fleet, "block": -1, "dbs": {}, "roots": {}}

    def known(self):
        """the fleets this store follows: a directory f/<fleet>/ with its state (Disk.list names one level)"""
        return [k for k in self.disk.list("f") if self.disk.json(self._key(k))]

    # --- the chain's side ---
    def follow(self, fleet):
        """read the fleet's logs since the last block seen; then try the roots whose manifest was missing"""
        with self.lock:
            st = self.state(fleet)
            try:
                latest = int(self.chain.call("eth_blockNumber"), 16)
                if latest > st["block"]:
                    logs = self.chain.call("eth_getLogs", {"address": fleet, "fromBlock": hex(st["block"] + 1), "toBlock": hex(latest)})
                    for log in logs:
                        self._apply(st, log)
                    st["block"] = latest
            except (RpcError, OSError, ValueError) as e:
                raise ChainUnreadable(str(e)[:200])
            self._check_roots(fleet, st)
            self.disk.set_json(self._key(fleet), st)
            return st

    def _apply(self, st, log):
        name = EVENTS.get(log["topics"][0][2:])
        data = log["data"][2:]
        if name is None:
            return
        if name == "RootPosted":
            seq = str(int(log["topics"][1], 16))
            st["roots"][seq] = {"root": "0x" + _w(data, 0), "manifest": "0x" + _w(data, 1), "state": "manifest missing"}
            return
        db = log["topics"][1].lower()
        d = st["dbs"].setdefault(db, {"epoch": 0})
        if name == "Admitted":
            d.update({"owner": "0x" + log["topics"][2][-40:], "sequencer": "0x" + _w(data, 0)[-40:], "nameHash": "0x" + _w(data, 1),
                      "instance": "0x" + "00" * 32, "retired": False})
        elif name == "Claimed":
            d["instance"] = "0x" + _w(data, 0)
        elif name == "EpochMoved":
            d["epoch"], d["epochSince"] = int(_w(data, 0), 16), int(_w(data, 1), 16)
        elif name == "SequencerSet":
            d["sequencer"], d["instance"] = "0x" + _w(data, 0)[-40:], "0x" + "00" * 32
        elif name in ("Retired", "Promoted"):
            d["retired"], d["instance"] = True, "0x" + "00" * 32

    def read_db(self, fleet, db):
        """one database's record on chain, at once (a head ahead of what the store holds)"""
        try:
            r = self.chain.call("eth_call", {"to": fleet, "data": "0x" + selector("db(bytes32)") + db[2:]}, "latest") or "0x"
        except (RpcError, OSError, ValueError) as e:
            raise ChainUnreadable(str(e)[:200])
        r = r[2:]
        if len(r) < 64 * 15 or int(_w(r, 0), 16) == 0:
            return None
        # Db: owner, sequencer, nameHash, instance, until, epoch, epochSince, admittedAt, retired, claimNonce, ...
        return {"owner": "0x" + _w(r, 0)[-40:], "sequencer": "0x" + _w(r, 1)[-40:], "nameHash": "0x" + _w(r, 2), "instance": "0x" + _w(r, 3),
                "epoch": int(_w(r, 5), 16), "epochSince": int(_w(r, 6), 16), "retired": int(_w(r, 8), 16) != 0}

    def _check_roots(self, fleet, st):
        for seq in sorted((s for s, r in st["roots"].items() if r["state"] != "ok"), key=int):
            r = st["roots"][seq]
            digest = bytes.fromhex(r["manifest"][2:])
            data = self.disk.read("c/%s" % _cid(digest))
            if data is None:
                continue
            why = _manifest_problem(data, digest, r["root"], fleet, self.chain_id, int(seq))
            if why:
                r["state"] = "bad manifest: " + why
                continue
            r["state"] = "ok"
            for leaf in json.loads(data)["leaves"]:
                d = st["dbs"].setdefault(leaf["dbId"].lower(), {"epoch": 0})
                d["latestRoot"] = int(seq)
                d["rootHead"] = _head_digest_hex(fleet, self.chain_id, leaf["head"])


def _cid(digest):
    """CIDv1, raw, sha2-256 - what cid_of gives the bytes"""
    import hpplite_store
    return "b" + hpplite_store._base32_lower(b"\x01\x55\x12\x20" + digest)


def _head(h):
    return {k: (bytes.fromhex(h[k][2:]) if isinstance(h[k], str) else int(h[k])) for k in HEAD_FIELDS}


def _head_digest_hex(fleet, chain_id, h):
    return "0x" + F.head_digest(F.domain(chain_id, fleet), _head(h)).hex()


def _manifest_problem(data, digest, root, fleet, chain_id, seq):
    import hashlib
    if hashlib.sha256(data).digest() != digest:
        return "its sha-256 is not the root's manifest digest"
    m = json.loads(data)
    if m.get("protocol") != F.PROTOCOL or m.get("rollup", "").lower() != fleet or m.get("seq") != seq or m.get("chainId") != chain_id:
        return "it names another fleet, chain or sequence"
    dom = F.domain(chain_id, fleet)
    entries = []
    for leaf in m["leaves"]:
        h = _head(leaf["head"])
        db = bytes.fromhex(leaf["dbId"][2:])
        entries.append((db, F.leaf(db, leaf["owner"], bytes.fromhex(leaf["nameHash"][2:]), F.head_digest(dom, h))))
    if "0x" + F.root(entries).hex() != root:
        return "its leaves do not make the root posted"
    return None


# ============ the calls (Handler methods delegate here) ============


def fleet_get(h, path, query):
    parts = path.split("/")             # ['', 'fleet', <fleet>, ...]
    fleet = parts[2].lower()
    book = h.service.fleets
    st = book.state(fleet) if book.disk.json(book._key(fleet)) else book.follow(fleet)
    if len(parts) == 5 and parts[3] == "db":
        db = parts[4].lower()
        d = st["dbs"].get(db)
        if not d:
            return h.reply(404, {"error": "no such database in the fleet", "dbId": db})
        head = h.service.disk.json("f/%s/%s/head.json" % (fleet, db))
        return h.reply(200, {"dbId": db, "epoch": d.get("epoch", 0), "sequencer": d.get("sequencer"), "instance": d.get("instance"),
                             "retired": d.get("retired", False), "latestHead": head, "latestRoot": d.get("latestRoot")})
    if len(parts) == 4 and parts[3] == "heads":
        after = (query.get("after") or ["0x" + "00" * 32])[0].lower()
        limit = min(int((query.get("limit") or ["1000"])[0]), 1000)
        out = []
        for db in sorted(st["dbs"], key=lambda x: int(x, 16)):
            if int(db, 16) <= int(after, 16) and after != "0x" + "00" * 32:
                continue
            head = h.service.disk.json("f/%s/%s/head.json" % (fleet, db))
            if head and _head_digest_hex(fleet, book.chain_id, head["head"]) != st["dbs"][db].get("rootHead"):
                out.append(head)
                if len(out) >= limit:
                    break
        return h.reply(200, {"fleet": fleet, "heads": out})
    if len(parts) == 5 and parts[3] == "name":
        db, _, height = parts[4].partition("-")
        rec = h.service.disk.read("f/%s/%s/n/%s" % (fleet, db.lower(), height)) if height.isdigit() else None
        data = h.service.disk.read("c/%s" % rec[97:].decode()) if rec else None
        if data is None:
            return h.reply(404, {"error": "no such name"})
        return h.reply_bytes(data + rec[:97])
    return h.reply(404, {"error": "no such endpoint"})


def fleet_post(h, path, body):
    parts = path.split("/")
    fleet = parts[2].lower()
    book = h.service.fleets
    if h.service.args.price_per_write or h.service.args.price_per_mib_month:
        return h.reply(501, {"error": "fleet writes on a store that charges need BindDb (not yet in this store)"})
    if len(parts) == 4 and parts[3] == "root":
        st = book.follow(fleet)                    # a hint to look now; the body is trusted for nothing
        return h.reply(200, {"fleet": fleet, "roots": st["roots"]})
    if len(parts) == 4 and parts[3] == "manifest":
        return _post_manifest(h, fleet, body)
    if len(parts) == 5 and parts[3] == "name":
        return _post_name(h, fleet, parts[4], body)
    return h.reply(404, {"error": "no such endpoint"})


def _post_manifest(h, fleet, body):
    """the anchor's manifest, before its root: kept under its CID, signed by the fleet's anchor as Content(fleet, seq, keccak)"""
    import hpplite_store as S
    try:
        m = json.loads(body)
        seq = int(m["seq"])
    except (ValueError, KeyError, TypeError):
        return h.reply(400, {"error": "a manifest is hpplite-fleet/1's canonical JSON"})
    sig = S.sig_bytes(h.headers.get("X-HPPLite-Signature"))
    dom = eth.domain_separator("HPPLiteStore", "2", h.service.chain_id, h.service.args.pay_to)
    signer = eth.recover(eth.typed_digest(dom, S.CONTENT_TYPE, fleet, seq, eth.keccak256(body)), sig) if sig else ""
    try:
        r = h.service.fleets.chain.call("eth_call", {"to": fleet, "data": "0x" + selector("anchor()")}, "latest") or "0x"
    except (RpcError, OSError, ValueError) as e:
        raise ChainUnreadable(str(e)[:200])
    if not signer or len(r) < 66 or not S.same(signer, "0x" + r[-40:]):
        return h.reply(401, {"error": "X-HPPLite-Signature: the fleet's anchor's Content signature"})
    import hashlib
    cid = _cid(hashlib.sha256(body).digest())
    h.service.disk.write("c/%s" % cid, body)
    refs = h.service.disk.json("c/%s.refs" % cid) or {}
    refs[fleet] = seq
    h.service.disk.set_json("c/%s.refs" % cid, refs)
    return h.reply(200, {"cid": cid, "bytes": len(body)})


def _post_name(h, fleet, name, body):
    book = h.service.fleets
    db, _, hs = name.partition("-")
    db = db.lower()
    if not hs.isdigit() or len(db) != 66 or len(body) <= 97:
        return h.reply(400, {"error": "a fleet name is <dbId>-<height>, the body the bytes, the instance (32 bytes) and the Head signature (65)"})
    height = int(hs)
    try:
        head = json.loads(h.headers.get("X-HPPLite-Head") or "")
        hb = _head(head)
    except (ValueError, KeyError, TypeError, AttributeError):
        return h.reply(400, {"error": "X-HPPLite-Head: the head's fields as JSON"})
    data, instance, sig = body[:-97], "0x" + body[-97:-65].hex(), body[-65:]
    if head["dbId"].lower() != db or hb["toHeight"] != height or hb["fromHeight"] != height or head["instanceId"].lower() != instance:
        return h.reply(400, {"error": "the head names another database, height or instance than this write"})
    dom = F.domain(book.chain_id, fleet)
    signer = eth.recover(F.head_digest(dom, hb), sig)
    with book.lock:
        st = book.follow(fleet) if not book.disk.json(book._key(fleet)) else book.state(fleet)
        d = st["dbs"].get(db)
        if not d or d.get("epoch", 0) < hb["epoch"] or (d.get("instance") or "").lower() != instance:
            fresh = book.read_db(fleet, db)          # ahead of what the store holds: the chain at once
            if fresh:
                d = st["dbs"].setdefault(db, {"epoch": 0})
                d.update(fresh)
                book.disk.set_json(book._key(fleet), st)
        if not d or not d.get("sequencer"):
            return h.reply(404, {"error": "no such database in the fleet", "dbId": db})
        if d.get("retired"):
            return h.reply(410, {"error": "the database is retired"})
        if not signer or not eth_same(signer, d["sequencer"]):
            return h.reply(403, {"error": "the head is not signed by the database's sequencer", "signer": signer, "sequencer": d["sequencer"]})
        if hb["epoch"] != d["epoch"] or (d.get("instance") or "").lower() != instance:
            return h.reply(409, {"error": "not the lease's epoch and instance", "epoch": hb["epoch"], "leaseEpoch": d["epoch"],
                                 "instance": instance, "leaseInstance": d.get("instance")})
        prev = h.service.disk.json("f/%s/%s/head.json" % (fleet, db))
        if prev and int(prev["head"]["toHeight"]) >= height:
            same = h.service.disk.read("f/%s/%s/n/%d" % (fleet, db, height))
            if same and same[:97] == body[-97:]:
                return h.reply(200, {"name": name, "bytes": len(data), "held": True})
            return h.reply(409, {"error": "the database's head is already past this height", "head": prev["head"]["toHeight"]})
        if prev:
            if int(prev["head"]["toHeight"]) != height - 1:
                return h.reply(409, {"error": "a height is missing before this one", "expected": int(prev["head"]["toHeight"]) + 1})
            before = bytes.fromhex(prev["head"]["chainHashAfter"][2:])
        elif height == 1:
            before = ZERO                              # a database that began in the fleet
        else:
            return h.reply(409, {"error": "this store holds no head of the database before this height (a migrated database's genesis is not read here yet)",
                                 "expected": 1})
        if chain_hash_next(before, height, eth.keccak256(data)) != hb["chainHashAfter"]:
            return h.reply(400, {"error": "the head's chain hash does not follow from the previous head and these bytes"})
        import hashlib
        cid = _cid(hashlib.sha256(data).digest())
        h.service.disk.write("c/%s" % cid, data)
        refs = h.service.disk.json("c/%s.refs" % cid) or {}
        refs["%s:%s" % (fleet, db)] = height
        h.service.disk.set_json("c/%s.refs" % cid, refs)
        h.service.disk.write("f/%s/%s/n/%d" % (fleet, db, height), body[-97:] + cid.encode())
        h.service.disk.set_json("f/%s/%s/head.json" % (fleet, db), {"head": head, "signature": "0x" + sig.hex()})
    return h.reply(200, {"name": name, "bytes": len(data), "cid": cid})


def eth_same(a, b):
    return a.lower() == b.lower()
