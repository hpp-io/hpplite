"""The chain over JSON-RPC, shared by the store, the relay and the mock
facilitator: eth_call views, the EIP-1559 transaction a relay signs, a
revert's reason. Standard library only.
"""
import json
import urllib.request

from . import eth


def selector(sig: str) -> str:
    return eth.keccak256(sig.encode())[:4].hex()


# ============ RLP and the EIP-1559 transaction ============

def rlp(item) -> bytes:
    if isinstance(item, list):
        body = b"".join(rlp(i) for i in item)
        return _rlp_len(len(body), 0xc0) + body
    if isinstance(item, int):
        item = item.to_bytes((item.bit_length() + 7) // 8, "big") if item else b""
    if len(item) == 1 and item[0] < 0x80:
        return item
    return _rlp_len(len(item), 0x80) + item


def _rlp_len(n, offset):
    if n < 56:
        return bytes([offset + n])
    b = n.to_bytes((n.bit_length() + 7) // 8, "big")
    return bytes([offset + 55 + len(b)]) + b


def signed_tx(key: bytes, chain_id, nonce, tip, max_fee, gas, to, data: bytes) -> bytes:
    fields = [chain_id, nonce, tip, max_fee, gas, bytes.fromhex(to[2:]), 0, data, []]
    sig = eth.sign(key, eth.keccak256(b"\x02" + rlp(fields)))
    r, s, v = int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:64], "big"), sig[64] - 27
    return b"\x02" + rlp(fields + [v, r, s])


# ============ The chain ============

class RpcError(Exception):
    def __init__(self, message, data=None):
        super().__init__(message)
        self.data = data


class Chain:
    def __init__(self, url, agent="hpplite-operators/1"):
        self.url = url
        self.agent = agent
        self._id = 0

    def call(self, method, *params):
        self._id += 1
        body = json.dumps({"jsonrpc": "2.0", "id": self._id, "method": method, "params": list(params)}).encode()
        req = urllib.request.Request(self.url, data=body, headers={"Content-Type": "application/json",
                                                                   "User-Agent": self.agent})
        with urllib.request.urlopen(req, timeout=60) as r:
            out = json.loads(r.read())
        if "error" in out:
            raise RpcError(out["error"].get("message", "error"), out["error"].get("data"))
        return out["result"]

    def view(self, to, sig, *words) -> str:
        data = "0x" + selector(sig) + "".join(eth.word(w).hex() for w in words)
        return self.call("eth_call", {"to": to, "data": data}, "latest")


def revert_reason(err: RpcError) -> str:
    """Error(string)'s message out of a revert's data, else the RPC's own text."""
    data = err.data if isinstance(err.data, str) else (err.data or {}).get("data") if isinstance(err.data, dict) else None
    if isinstance(data, str) and data.startswith("0x08c379a0") and len(data) >= 138:
        try:
            n = int(data[74:138], 16)
            return bytes.fromhex(data[138:138 + 2 * n]).decode("utf-8", "replace")
        except ValueError:
            pass
    return str(err)


ERC1271_MAGIC = "1626ba7e"      # isValidSignature(bytes32,bytes)'s answer for a signature the account accepts


class ChainUnreadable(Exception):
    """The chain could not be asked (unreachable, or answering an error): not the same as 'no rollup' (#198)"""


def signed_by(chain, account, digest: bytes, sig: bytes) -> bool:
    """A funder's signature (#356): it recovers to the account, or - the account being a contract, a smart wallet -
    the account's ERC-1271 isValidSignature(digest, sig) answers 0x1626ba7e. A plain key's signature never asks the
    chain; without a chain only recovery counts. The chain unreadable raises ChainUnreadable: the caller answers 503"""
    if len(sig) == 65:
        try:
            if eth.recover(digest, sig).lower() == account.lower():
                return True
        except ValueError:
            pass
    if chain is None or not sig:
        return False
    try:
        if chain.call("eth_getCode", account, "latest") in (None, "0x", "0x0"):
            return False
        tail = len(sig).to_bytes(32, "big") + sig + b"\0" * (-len(sig) % 32)
        data = "0x" + selector("isValidSignature(bytes32,bytes)") + digest.hex() + (64).to_bytes(32, "big").hex() + tail.hex()
        out = chain.call("eth_call", {"to": account, "data": data}, "latest") or ""
    except RpcError:
        return False                    # the account reverted: it does not accept the signature
    except (OSError, ValueError) as e:
        raise ChainUnreadable(str(e)[:200])
    return out[2:10] == ERC1271_MAGIC

