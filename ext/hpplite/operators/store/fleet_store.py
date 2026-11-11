"""A store's side of hpplite-fleet/1 (#297, doc/fleet-protocol.md, "The store"): the fleet's state from its events,
the name write that carries a database's signed head (checked against the database's sequencer, lease and epoch -
the epoch is the fence), the heads an anchor builds the next root from, and the latest root per database from
RootPosted with the manifest checked against digest and root.

A fleet database's bucket (#310, #311) is a bucket of the store's ledger under bucket_key(fleet, dbId), bound with
BindDb and released, deleted and capped with its siblings; its name writes are charged to it as a rollup's are.
"""
import json
import os
import sys
import threading
import time

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

    def __init__(self, chain, disk, chain_id, max_age=5.0):
        self.chain, self.disk, self.chain_id = chain, disk, chain_id
        self.max_age = max_age          # a view older than this is followed again before a write is checked or a db shown (#317)
        self.lock = threading.RLock()
        self.read_at = {}

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
            self.read_at[fleet] = time.time()
            return st

    def fresh(self, fleet):
        """the fleet's state, followed again when the last read is older than max_age (or never was, in this run):
        an old holder's write after a takeover is refused within max_age plus a block, not at the next tick (#317)"""
        with self.lock:
            if self.disk.json(self._key(fleet)) and time.time() - self.read_at.get(fleet, 0) < self.max_age:
                return self.state(fleet)
            return self.follow(fleet)

    def _apply(self, st, log):
        name = EVENTS.get(log["topics"][0][2:])
        data = log["data"][2:]
        if name is None:
            return
        if name == "RootPosted":
            seq = str(int(log["topics"][1], 16))
            st["roots"][seq] = {"root": "0x" + _w(data, 0), "manifest": "0x" + _w(data, 1), "state": "manifest missing",
                                "block": log.get("blockNumber")}
            return
        db = log["topics"][1].lower()
        d = st["dbs"].setdefault(db, {"epoch": 0})
        if name == "Admitted":
            d.update({"owner": "0x" + log["topics"][2][-40:], "sequencer": "0x" + _w(data, 0)[-40:], "nameHash": "0x" + _w(data, 1),
                      "instance": "0x" + "00" * 32, "retired": False, "admittedAt": None})     # a new life (#318): its admittedAt read from the record
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
                "epoch": int(_w(r, 5), 16), "epochSince": int(_w(r, 6), 16), "admittedAt": int(_w(r, 7), 16), "retired": int(_w(r, 8), 16) != 0}

    def drop_manifest(self, fleet, cid):
        """a manifest no root names: its ref goes, and its bytes with the last ref"""
        refs = self.disk.json("c/%s.refs" % cid) or {}
        refs.pop(fleet, None)
        if refs:
            self.disk.set_json("c/%s.refs" % cid, refs)
        else:
            self.disk.delete("c/%s.refs" % cid)
            self.disk.delete("c/%s" % cid)

    def sweep_manifests(self, now, age):
        """manifests whose root never reached the chain, once older than age; one an indexed root names is kept"""
        with self.lock:
            for fleet in self.known():
                pend = self.disk.json("f/%s/manifests.json" % fleet) or {}
                if not pend:
                    continue
                st = self.state(fleet)
                for seq in list(pend):
                    r = st["roots"].get(seq)
                    if r and r.get("state") == "ok" and _cid(bytes.fromhex(r["manifest"][2:])) == pend[seq]["cid"]:
                        del pend[seq]                  # its root is on chain and indexed: kept as any root's
                    elif now - pend[seq]["at"] >= age:
                        self.drop_manifest(fleet, pend[seq]["cid"])
                        del pend[seq]
                self.disk.set_json("f/%s/manifests.json" % fleet, pend)

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
            try:
                blk = self.chain.call("eth_getBlockByNumber", r.get("block") or "latest", False) or {}
                when = int(blk.get("timestamp", "0x0"), 16)
            except (RpcError, OSError, ValueError):
                when = None
            leaves = json.loads(data)["leaves"]
            dom = F.domain(self.chain_id, fleet)
            entries = [(bytes.fromhex(l["dbId"][2:]), F.leaf(bytes.fromhex(l["dbId"][2:]), l["owner"], bytes.fromhex(l["nameHash"][2:]),
                                                             F.head_digest(dom, _head(l["head"])))) for l in leaves]
            for leaf in leaves:
                d = st["dbs"].setdefault(leaf["dbId"].lower(), {"epoch": 0})
                if d.get("latestRoot") and d["latestRoot"] >= int(seq):
                    continue                           # a root indexed late never moves a database back
                d["latestRoot"] = int(seq)
                d["rootHead"] = _head_digest_hex(fleet, self.chain_id, leaf["head"])
                path = F.proof(entries, bytes.fromhex(leaf["dbId"][2:]))
                # the database's latest root that holds it: what somaro's status shows and verify checks on chain - the leaf's
                # path into the root, siblings bottom up with their side; a database absent from a later root keeps it
                d["anchored"] = {"seq": int(seq), "root": r["root"], "toHeight": int(leaf["head"]["toHeight"]), "time": when,
                                 "path": [{"hash": "0x" + hsh.hex(), "left": left} for hsh, left in path]}


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
    st = book.fresh(fleet)
    if len(parts) == 5 and parts[3] == "db":
        db = parts[4].lower()
        d = st["dbs"].get(db)
        if not d:
            return h.reply(404, {"error": "no such database in the fleet", "dbId": db})
        admitted = _admission(book, st, fleet, db)
        head = h.service.disk.json(_hpath(fleet, db, admitted)) if admitted is not None else None
        out = {"dbId": db, "epoch": d.get("epoch", 0), "sequencer": d.get("sequencer"), "instance": d.get("instance"),
               "retired": d.get("retired", False), "admittedAt": admitted, "latestHead": head, "latestRoot": d.get("latestRoot")}
        if d.get("anchored"):
            out["anchored"] = d["anchored"]             # absent before the first indexed root that holds it
        return h.reply(200, out)
    if len(parts) == 6 and parts[3] == "db" and parts[5] == "bucket":
        b = h.L.bucket(bucket_key(fleet, parts[4].lower()))
        if not b:
            return h.reply(404, {"error": "no such bucket", "fleet": fleet, "dbId": parts[4].lower()})
        a = h.L.account(b["funder"]) if b.get("funder") else None
        out = {"fleet": fleet, "dbId": parts[4].lower()}
        out.update({k: b.get(k) for k in ("funder", "state", "bytes", "objects", "height", "pruneHeight", "parkedAt")})
        out["coveredUntil"] = h.L.covered_until(a) if a and b["state"] == "covered" else 0
        out["cap"], out["today"] = b.get("cap"), h.L.today(b)
        return h.reply(200, out)
    if len(parts) == 4 and parts[3] == "heads":
        after = (query.get("after") or ["0x" + "00" * 32])[0].lower()
        limit = min(int((query.get("limit") or ["1000"])[0]), 1000)
        out = []
        for db in sorted(st["dbs"], key=lambda x: int(x, 16)):
            if int(db, 16) <= int(after, 16) and after != "0x" + "00" * 32:
                continue
            admitted = _admission(book, st, fleet, db)        # the current life's head (#318)
            head = h.service.disk.json(_hpath(fleet, db, admitted)) if admitted is not None else None
            if head and _head_digest_hex(fleet, book.chain_id, head["head"]) != st["dbs"][db].get("rootHead"):
                out.append(head)
                if len(out) >= limit:
                    break
        return h.reply(200, {"fleet": fleet, "heads": out})
    if len(parts) == 5 and parts[3] == "name":
        parsed = parse_name(parts[4])                 # any admission's: an old life stays readable (#318)
        rec = h.service.disk.read(_npath(fleet, *parsed)) if parsed else None
        data = h.service.disk.read("c/%s" % rec[97:].decode()) if rec else None
        if data is None:
            return h.reply(404, {"error": "no such name"})
        return h.reply_bytes(data + rec[:97])
    return h.reply(404, {"error": "no such endpoint"})


def fleet_post(h, path, body):
    parts = path.split("/")
    fleet = parts[2].lower()
    book = h.service.fleets
    if len(parts) >= 6 and parts[3] == "db" and parts[5] == "bucket":
        db = parts[4].lower()
        if not re_hex32(db):
            return h.reply(400, {"error": "a dbId is 32 bytes of hex"})
        if len(parts) == 6:
            return _post_binddb(h, fleet, db, body)
        if len(parts) == 7 and parts[6] == "cap":
            return _post_capdb(h, fleet, db, body)
        if len(parts) == 7 and parts[6] == "release":
            return _post_releasedb(h, fleet, db, body)
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
    # manifests are free (somaro): so only one per root not yet posted - a seq the chain has posted is 409, and a newer
    # manifest for an unposted seq replaces the one held (an anchor whose postRootFor failed rebuilds); one no root names
    # is swept after --sweep-age
    try:
        posted = int(h.service.fleets.chain.call("eth_call", {"to": fleet, "data": "0x" + selector("rootSeq()")}, "latest") or "0x0", 16)
    except (RpcError, OSError, ValueError) as e:
        raise ChainUnreadable(str(e)[:200])
    import hashlib
    cid = _cid(hashlib.sha256(body).digest())
    book = h.service.fleets
    if seq <= posted:
        # a posted root's own manifest, put late, is still taken: the one whose sha-256 the root names, while it is missing
        r = book.follow(fleet)["roots"].get(str(seq))
        if not r or r.get("state") != "manifest missing" or r["manifest"] != "0x" + hashlib.sha256(body).hexdigest():
            return h.reply(409, {"error": "root %d is posted already: a manifest is taken only for the next root, or as the posted root's own" % seq,
                                 "rootSeq": posted})
        h.service.disk.write("c/%s" % cid, body)
        refs = h.service.disk.json("c/%s.refs" % cid) or {}
        refs[fleet] = seq
        h.service.disk.set_json("c/%s.refs" % cid, refs)
        book.follow(fleet)                            # indexed now
        return h.reply(200, {"cid": cid, "bytes": len(body)})
    with book.lock:
        pend = book.disk.json("f/%s/manifests.json" % fleet) or {}
        old = pend.get(str(seq))
        if old and old["cid"] != cid:
            book.drop_manifest(fleet, old["cid"])
        h.service.disk.write("c/%s" % cid, body)
        refs = h.service.disk.json("c/%s.refs" % cid) or {}
        refs[fleet] = seq
        h.service.disk.set_json("c/%s.refs" % cid, refs)
        pend[str(seq)] = {"cid": cid, "at": int(time.time())}
        book.disk.set_json("f/%s/manifests.json" % fleet, pend)
    return h.reply(200, {"cid": cid, "bytes": len(body)})


def _npath(fleet, db, admitted, height):
    """a name's record at the store, by its admission (#318)"""
    return "f/%s/%s/%d/n/%d" % (fleet, db, admitted, height)


def _hpath(fleet, db, admitted):
    """an admission's latest head (#318)"""
    return "f/%s/%s/%d/head.json" % (fleet, db, admitted)


def _admission(book, st, fleet, db):
    """the database's current admission, the record's admittedAt (#318): read on chain when the store does not hold it"""
    d = st["dbs"].get(db)
    if d is None:
        return None
    if d.get("admittedAt") is None:
        fresh = book.read_db(fleet, db)
        if fresh:
            d.update(fresh)
            book.disk.set_json(book._key(fleet), st)
    return d.get("admittedAt")


def parse_name(name):
    """<dbId>-<admittedAt>-<height> (#318): (dbId lowercase, admittedAt, height), or None"""
    parts = name.split("-")
    if len(parts) != 3 or len(parts[0]) != 66 or not parts[1].isdigit() or not parts[2].isdigit():
        return None
    return parts[0].lower(), int(parts[1]), int(parts[2])


def _post_name(h, fleet, name, body):
    book = h.service.fleets
    parsed = parse_name(name)
    if not parsed or len(body) <= 97:
        return h.reply(400, {"error": "a fleet name is <dbId>-<admittedAt>-<height>, the body the bytes, the instance (32 bytes) and the Head signature (65)"})
    db, admitted, height = parsed
    try:
        head = json.loads(h.headers.get("X-HPPLite-Head") or "")
        hb = _head(head)
    except (ValueError, KeyError, TypeError, AttributeError):
        return h.reply(400, {"error": "X-HPPLite-Head: the head's fields as JSON"})
    data, instance, sig = body[:-97], "0x" + body[-97:-65].hex(), body[-65:]
    if head["dbId"].lower() != db or hb["toHeight"] != height or hb["fromHeight"] != height or head["instanceId"].lower() != instance:
        return h.reply(400, {"error": "the head names another database, height or instance than this write"})
    import hashlib
    if hb["manifestDigest"] != hashlib.sha256(data).digest():
        # a batch's head commits to its bytes' sha-256 (its CID's digest): a root then commits to what the store holds (#315)
        return h.reply(400, {"error": "the head's manifestDigest is not the sha-256 of the bytes written with it"})
    dom = F.domain(book.chain_id, fleet)
    signer = eth.recover(F.head_digest(dom, hb), sig)
    with book.lock:
        st = book.fresh(fleet)
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
            fresh = book.read_db(fleet, db)          # a refusal reads the record first: never refused on a view the chain has left (#317)
            if fresh:
                d.update(fresh)
                book.disk.set_json(book._key(fleet), st)
        if hb["epoch"] != d["epoch"] or (d.get("instance") or "").lower() != instance:
            return h.reply(409, {"error": "not the lease's epoch and instance", "epoch": hb["epoch"], "leaseEpoch": d["epoch"],
                                 "instance": instance, "leaseInstance": d.get("instance")})
        current = _admission(book, st, fleet, db)
        if admitted != current:
            fresh = book.read_db(fleet, db)          # as for the epoch: never refused on a view the chain has left
            if fresh:
                d.update(fresh)
                book.disk.set_json(book._key(fleet), st)
                current = d.get("admittedAt")
        if admitted != current:
            return h.reply(409, {"error": "not the database's current admission", "admittedAt": admitted, "leaseAdmittedAt": current})
        prev = h.service.disk.json(_hpath(fleet, db, admitted))
        if prev and int(prev["head"]["toHeight"]) >= height:
            same = h.service.disk.read(_npath(fleet, db, admitted, height))
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
        key = bucket_key(fleet, db)

        def work():
            h.service.disk.write("c/%s" % cid, data)
            refs = h.service.disk.json("c/%s.refs" % cid) or {}
            refs[key] = height
            h.service.disk.set_json("c/%s.refs" % cid, refs)
            b = h.L.bucket(key)
            if b:
                b.setdefault("content", {})[cid] = {"height": height, "bytes": len(data)}
                b["bytes"] += len(data) + 97
                b["objects"] += 1
                b["height"] = height                   # the current life's (#318)
                h.L.save_bucket(b)
            h.service.disk.write(_npath(fleet, db, admitted, height), body[-97:] + cid.encode())
            h.service.disk.set_json(_hpath(fleet, db, admitted), {"head": head, "signature": "0x" + sig.hex(), "admittedAt": admitted})
            return 200, {"name": name, "bytes": len(data), "cid": cid}
        if h.L.free:
            status, obj = work()
            return h.reply(status, obj)
    # a store that charges: the database's bucket pays, as a rollup's does (unbound: the same 402)
    return h.write(h.L.bucket(key), len(data) + 97, lambda: _locked(book, work))


def _locked(book, work):
    with book.lock:
        return work()


def eth_same(a, b):
    return a.lower() == b.lower()


# ============ money: a fleet database's bucket (#310, #311) ============


def bucket_key(fleet, db):
    """the ledger's key for a fleet database's bucket: apart from every rollup's DA"""
    return "fd-%s-%s" % (fleet[2:].lower(), db[2:].lower())


def _db_bucket(h, fleet, db):
    b = h.L.bucket(bucket_key(fleet, db))
    return b if b and b["state"] != "deleted" and b.get("funder") else None


def _post_binddb(h, fleet, db, body):
    """BindDb(fleet, dbId, funder, issuedAt): the funder, and the database's side - with a record its sequencer or its
    owner alone; before the record its owner alone, the request naming owner and nameHash (the derivation checked)"""
    import time
    import hpplite_store as S
    req = h.body_json(body)
    if not req or not S.ADDR_RE.match(str(req.get("funder", ""))) or not isinstance(req.get("issuedAt"), int):
        return h.reply(400, {"error": "a binding is {funder, issuedAt, funderSignature, dbSignature[, owner, nameHash]}"})
    funder, issued = req["funder"].lower(), req["issuedAt"]
    digest = eth.typed_digest(h.L.domain, S.BINDDB_TYPE, fleet, bytes.fromhex(db[2:]), funder, issued)
    f_sig, d_sig = S.sig_bytes(req.get("funderSignature")), S.sig_bytes(req.get("dbSignature"))
    if not f_sig or not eth_same(eth.recover(digest, f_sig), funder):
        return h.reply(401, {"error": "the funder's signature does not recover to the funder"})
    signer = eth.recover(digest, d_sig) if d_sig else ""
    st = h.service.fleets.follow(fleet)
    d = st["dbs"].get(db)
    if d and d.get("owner"):
        allowed = [d["owner"]] + ([d["sequencer"]] if d.get("sequencer") else [])
        if not signer or not any(eth_same(signer, x) for x in allowed):
            return h.reply(401, {"error": "the database's side: its sequencer or its owner must sign", "signer": signer})
    else:
        owner, nh = str(req.get("owner", "")).lower(), str(req.get("nameHash", ""))
        if not S.ADDR_RE.match(owner) or not re_hex32(nh):
            return h.reply(400, {"error": "a database the fleet has no record of yet: name owner and nameHash beside the owner's signature"})
        if "0x" + eth.keccak256(eth.word(owner) + eth.word(bytes.fromhex(nh[2:]))).hex() != db:
            return h.reply(400, {"error": "dbId is not keccak256(abi.encode(owner, nameHash))"})
        if not signer or not eth_same(signer, owner):
            return h.reply(401, {"error": "before its record, the database's owner alone binds it", "signer": signer})
    key = bucket_key(fleet, db)
    with h.L.lock:
        b = h.L.bucket(key)
        if b and b["state"] != "deleted" and not h.fresh(issued, b.get("boundAt")):
            return h.reply(409, {"error": "a binding no newer than the last taken", "last": b.get("boundAt")})
        now = int(time.time())
        if not b or b["state"] == "deleted":
            b = {"da": key, "fleet": fleet, "dbId": db, "bytes": 0, "objects": 0, "height": 0, "pruneHeight": 0, "content": {},
                 "names": {}, "created": now, "accruedAt": now, "pruneAt": 0}
        else:
            h.L.accrue(b, now)
        if b.get("funder") and not eth_same(b.get("funder"), funder):
            b["cap"], b["capAt"] = None, 0                 # the cap goes with the funder (#274)
        b.update({"funder": funder, "binder": signer.lower(), "boundAt": issued, "state": "covered", "parkedAt": None})
        h.L.save_bucket(b)
        a = h.L.account(funder, create=True)
        h.L.save_account(a)
        if a.get("uncovered") or (not h.L.free and a["credit"] < 0 and not a.get("permit")):
            b["state"], b["parkedAt"] = "parked", now
            h.L.save_bucket(b)
        return h.reply(200, {"fleet": fleet, "dbId": db, "funder": funder, "state": b["state"]})


def _post_capdb(h, fleet, db, body):
    import hpplite_store as S
    req = h.body_json(body)
    try:
        writes, nbytes, issued = int(req["writesPerDay"]), int(req["bytesPerDay"]), int(req["issuedAt"])
        funder = str(req["funder"]).lower()
    except (KeyError, TypeError, ValueError):
        return h.reply(400, {"error": "a cap is {funder, writesPerDay, bytesPerDay, issuedAt, signature}"})
    with h.L.lock:
        b = _db_bucket(h, fleet, db)
        if not b:
            return h.reply(404, {"error": "no bound bucket", "fleet": fleet, "dbId": db})
        digest = eth.typed_digest(h.L.domain, S.CAPDB_TYPE, fleet, bytes.fromhex(db[2:]), writes, nbytes, issued)
        sig = S.sig_bytes(req.get("signature"))
        if not sig or not eth_same(funder, b["funder"]) or not eth_same(eth.recover(digest, sig), b["funder"]):
            return h.reply(401, {"error": "not signed by the bucket's funder"})
        if not h.fresh(issued, b.get("capAt")):
            return h.reply(409, {"error": "a cap no newer than the last taken", "last": b.get("capAt")})
        b["cap"] = {"writesPerDay": writes, "bytesPerDay": nbytes} if (writes or nbytes) else None
        b["capAt"] = issued
        h.L.save_bucket(b)
        return h.reply(200, {"fleet": fleet, "dbId": db, "cap": b["cap"], "today": h.L.today(b)})


def _post_releasedb(h, fleet, db, body):
    import time
    import hpplite_store as S
    req = h.body_json(body)
    if not req or not isinstance(req.get("issuedAt"), int):
        return h.reply(400, {"error": "a release is {funder, issuedAt, signature[, delete]}"})
    with h.L.lock:
        b = _db_bucket(h, fleet, db)
        if not b:
            return h.reply(404, {"error": "no bound bucket", "fleet": fleet, "dbId": db})
        delete = req.get("delete") is True
        digest = eth.typed_digest(h.L.domain, S.DELETEDB_TYPE if delete else S.RELEASEDB_TYPE, fleet, bytes.fromhex(db[2:]),
                                  b["funder"], req["issuedAt"])
        sig = S.sig_bytes(req.get("signature"))
        if not sig or not eth_same(eth.recover(digest, sig), b["funder"]) or not eth_same(str(req.get("funder", "")), b["funder"]):
            return h.reply(401, {"error": "not signed by the bucket's funder"})
        if not h.fresh(req["issuedAt"], b.get("boundAt")):
            return h.reply(409, {"error": "a release no newer than the binding"})
        now = int(time.time())
        h.L.accrue(b, now)
        b.update({"funder": None, "boundAt": req["issuedAt"], "state": "parked", "parkedAt": now})
        h.L.save_bucket(b)
        if delete:
            h.L.delete_bucket(b)
            return h.reply(200, {"fleet": fleet, "dbId": db, "funder": None, "state": "deleted"})
        return h.reply(200, {"fleet": fleet, "dbId": db, "funder": None, "state": "parked"})


def re_hex32(x):
    return isinstance(x, str) and len(x) == 66 and x.startswith("0x") and all(c in "0123456789abcdefABCDEF" for c in x[2:])
