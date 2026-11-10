"""The asset a store or a relay is paid in, read and pulled from (#214, #276): balance, allowance, the permit's
nonce; permit and transferFrom sent by the service's spender key, each simulated first so a refusal costs no gas.
Shared by operators/store/hpplite_store.py and operators/relay/relay.py."""
import threading
import time

from . import eth
from .chain import RpcError, revert_reason, selector, signed_tx


def sig_bytes(h):
    """A 65-byte signature from 0x-hex, or None"""
    try:
        b = bytes.fromhex(h[2:] if isinstance(h, str) and h.startswith("0x") else "")
    except ValueError:
        return None
    return b if len(b) == 65 else None


class ChainUnreadable(Exception):
    """The chain could not be asked (unreachable, or answering an error): not the same as 'no rollup' (#198)"""


class Token:
    """The asset, read and pulled from: balance, allowance, the permit's nonce; permit and transferFrom sent by the
    service's spender, each simulated first so a refusal costs no gas"""

    def __init__(self, chain, asset, spender_key, chain_id, receipt_timeout=60, lock=None):
        self.chain, self.asset, self.key, self.chain_id = chain, asset, spender_key, chain_id
        self.spender = eth.address(spender_key)
        self.receipt_timeout = receipt_timeout
        # the spender's nonces, one transaction at a time: the service's own lock when its key signs others too (a relay)
        self.lock = lock or threading.Lock()

    def _uint(self, sig, *words):
        try:
            return int(self.chain.view(self.asset, sig, *words) or "0x0", 16)
        except RpcError as e:
            raise ChainUnreadable(str(e)[:200])
        except (OSError, ValueError) as e:
            raise ChainUnreadable(str(e)[:200])

    def balance(self, a):
        return self._uint("balanceOf(address)", a)

    def allowance(self, a):
        return self._uint("allowance(address,address)", a, self.spender)

    def nonce(self, a):
        return self._uint("nonces(address)", a)

    @staticmethod
    def permit_data(owner, p):
        sig = sig_bytes(p["signature"])
        v, r, s = sig[64], sig[:32], sig[32:64]
        return "0x" + selector("permit(address,address,uint256,uint256,uint8,bytes32,bytes32)") + "".join(
            eth.word(w).hex() for w in (owner, p["spender"], int(p["value"]), int(p["deadline"]), int(v), r, s))

    def transfer_data(self, owner, to, amount):
        return "0x" + selector("transferFrom(address,address,uint256)") + "".join(eth.word(w).hex() for w in (owner, to, amount))

    def send(self, data):
        """Simulate, then send and wait. ('ok', tx) | ('refused', reason) | ('failed', reason): refused is the chain
        saying no to the call itself (the funder's balance, allowance, permit); failed is anything else"""
        call = {"from": self.spender, "to": self.asset, "data": data}
        try:
            self.chain.call("eth_call", call, "latest")
        except RpcError as e:
            # the call itself reverting is the funder's refusal; the RPC failing (a rate limit, an upstream error) is
            # the store's own trouble, which never parks a bucket (#220)
            if e.data or "revert" in str(e).lower():
                return "refused", revert_reason(e)
            return "failed", "simulation: %s" % revert_reason(e)
        except (OSError, ValueError) as e:
            return "failed", "simulation: %s" % e
        with self.lock:
            try:
                gas = int(self.chain.call("eth_estimateGas", call), 16) * 2
                base = int(self.chain.call("eth_getBlockByNumber", "latest", False).get("baseFeePerGas", "0x0"), 16)
                try:
                    tip = int(self.chain.call("eth_maxPriorityFeePerGas"), 16)
                except RpcError:
                    tip = 0
                nonce = int(self.chain.call("eth_getTransactionCount", self.spender, "pending"), 16)
                raw = signed_tx(self.key, self.chain_id, nonce, tip, 2 * base + tip, gas, self.asset, bytes.fromhex(data[2:]))
                tx = self.chain.call("eth_sendRawTransaction", "0x" + raw.hex())
            except RpcError as e:
                return "failed", "submission: %s" % revert_reason(e)
            except (OSError, ValueError) as e:
                return "failed", "submission: %s" % e
            deadline = time.time() + self.receipt_timeout
            while time.time() < deadline:
                try:
                    rcpt = self.chain.call("eth_getTransactionReceipt", tx)
                except (RpcError, OSError, ValueError):
                    rcpt = None
                if rcpt:
                    if int(rcpt.get("status", "0x0"), 16) == 1:
                        return "ok", tx
                    return "failed", "reverted on chain: %s" % tx   # passed the simulation: the store's to retry
                time.sleep(0.3)
            return "failed", "no receipt for %s" % tx
