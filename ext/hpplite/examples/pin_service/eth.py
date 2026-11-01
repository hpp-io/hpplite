"""What the pin service and its buyer need of Ethereum, in the standard
library only: keccak-256, secp256k1 signing and recovery (RFC 6979 nonces,
low-s), EIP-712 digests, and an address from a key. A reference, not a
library: slow by design, a few hundred signatures a second, which is far
more than a pin service signs.
"""
import hashlib
import hmac
import secrets

# ============ keccak-256 (the Ethereum one, not SHA3-256) ============

_RC = [
    0x0000000000000001, 0x0000000000008082, 0x800000000000808A, 0x8000000080008000,
    0x000000000000808B, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
    0x000000000000008A, 0x0000000000000088, 0x0000000080008009, 0x000000008000000A,
    0x000000008000808B, 0x800000000000008B, 0x8000000000008089, 0x8000000000008003,
    0x8000000000008002, 0x8000000000000080, 0x000000000000800A, 0x800000008000000A,
    0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
]
_ROT = [
    [0, 36, 3, 41, 18], [1, 44, 10, 45, 2], [62, 6, 43, 15, 61],
    [28, 55, 25, 21, 56], [27, 20, 39, 8, 14],
]
_MASK = (1 << 64) - 1


def _rol(x, n):
    return ((x << n) | (x >> (64 - n))) & _MASK


def _keccak_f(a):
    for rc in _RC:
        c = [a[x][0] ^ a[x][1] ^ a[x][2] ^ a[x][3] ^ a[x][4] for x in range(5)]
        d = [c[(x - 1) % 5] ^ _rol(c[(x + 1) % 5], 1) for x in range(5)]
        a = [[a[x][y] ^ d[x] for y in range(5)] for x in range(5)]
        b = [[0] * 5 for _ in range(5)]
        for x in range(5):
            for y in range(5):
                b[y][(2 * x + 3 * y) % 5] = _rol(a[x][y], _ROT[x][y])
        a = [[b[x][y] ^ ((~b[(x + 1) % 5][y]) & b[(x + 2) % 5][y]) for y in range(5)] for x in range(5)]
        a[0][0] ^= rc
    return a


def keccak256(data: bytes) -> bytes:
    rate = 136
    padded = bytearray(data) + b"\x01"
    padded += b"\x00" * ((rate - len(padded) % rate) % rate)
    padded[-1] |= 0x80
    a = [[0] * 5 for _ in range(5)]
    for off in range(0, len(padded), rate):
        block = padded[off:off + rate]
        for i in range(rate // 8):
            lane = int.from_bytes(block[8 * i:8 * i + 8], "little")
            a[i % 5][i // 5] ^= lane
        a = _keccak_f(a)
    out = b""
    for i in range(4):
        out += a[i % 5][i // 5].to_bytes(8, "little")
    return out


# ============ secp256k1 ============

_P = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEFFFFFC2F
_N = 0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
_G = (0x79BE667EF9DCBBAC55A06295CE870B07029BFCDB2DCE28D959F2815B16F81798,
      0x483ADA7726A3C4655DA4FBFC0E1108A8FD17B448A68554199C47D08FFB10D4B8)


def _add(p, q):
    if p is None:
        return q
    if q is None:
        return p
    (x1, y1), (x2, y2) = p, q
    if x1 == x2:
        if (y1 + y2) % _P == 0:
            return None
        lam = (3 * x1 * x1) * pow(2 * y1, -1, _P) % _P
    else:
        lam = (y2 - y1) * pow(x2 - x1, -1, _P) % _P
    x3 = (lam * lam - x1 - x2) % _P
    return (x3, (lam * (x1 - x3) - y1) % _P)


def _mul(k, p):
    r = None
    while k:
        if k & 1:
            r = _add(r, p)
        p = _add(p, p)
        k >>= 1
    return r


def pubkey(priv: bytes):
    return _mul(int.from_bytes(priv, "big"), _G)


def address(priv: bytes) -> str:
    x, y = pubkey(priv)
    return "0x" + keccak256(x.to_bytes(32, "big") + y.to_bytes(32, "big"))[12:].hex()


def _rfc6979_k(priv: int, h: bytes) -> int:
    x = priv.to_bytes(32, "big")
    v = b"\x01" * 32
    k = b"\x00" * 32
    k = hmac.new(k, v + b"\x00" + x + h, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    k = hmac.new(k, v + b"\x01" + x + h, hashlib.sha256).digest()
    v = hmac.new(k, v, hashlib.sha256).digest()
    while True:
        v = hmac.new(k, v, hashlib.sha256).digest()
        cand = int.from_bytes(v, "big")
        if 1 <= cand < _N:
            return cand
        k = hmac.new(k, v + b"\x00", hashlib.sha256).digest()
        v = hmac.new(k, v, hashlib.sha256).digest()


def sign(priv: bytes, digest: bytes) -> bytes:
    """65 bytes r || s || v (v = 27 or 28), low-s, as ecrecover expects."""
    d = int.from_bytes(priv, "big")
    z = int.from_bytes(digest, "big")
    while True:
        k = _rfc6979_k(d, digest)
        x, y = _mul(k, _G)
        r = x % _N
        if r == 0:
            digest = keccak256(digest)
            continue
        s = pow(k, -1, _N) * (z + r * d) % _N
        if s == 0:
            digest = keccak256(digest)
            continue
        rec = (y & 1) | (2 if x >= _N else 0)
        if s > _N // 2:
            s = _N - s
            rec ^= 1
        return r.to_bytes(32, "big") + s.to_bytes(32, "big") + bytes([27 + rec])


def recover(digest: bytes, sig: bytes) -> str:
    """The signer's address of a 65-byte signature, or '' if it is malformed."""
    if len(sig) != 65:
        return ""
    r = int.from_bytes(sig[:32], "big")
    s = int.from_bytes(sig[32:64], "big")
    v = sig[64]
    if v < 27:
        v += 27
    if v not in (27, 28) or not (1 <= r < _N) or not (1 <= s < _N):
        return ""
    x = r
    alpha = (pow(x, 3, _P) + 7) % _P
    y = pow(alpha, (_P + 1) // 4, _P)
    if y * y % _P != alpha:
        return ""
    if (y & 1) != (v - 27):
        y = _P - y
    z = int.from_bytes(digest, "big")
    rinv = pow(r, -1, _N)
    q = _add(_mul(s * rinv % _N, (x, y)), _mul((-z * rinv) % _N, _G))
    if q is None:
        return ""
    return "0x" + keccak256(q[0].to_bytes(32, "big") + q[1].to_bytes(32, "big"))[12:].hex()


# ============ EIP-712 ============

_DOMAIN_TYPEHASH = keccak256(b"EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)")


def word(v) -> bytes:
    """One ABI word: an int, an address string, 32 bytes, or a bytes value to hash."""
    if isinstance(v, int):
        return v.to_bytes(32, "big")
    if isinstance(v, str) and v.startswith("0x") and len(v) == 42:
        return bytes(12) + bytes.fromhex(v[2:])
    if isinstance(v, bytes) and len(v) == 32:
        return v
    raise TypeError("not a word: %r" % (v,))


def domain_separator(name: str, version: str, chain_id: int, contract: str) -> bytes:
    return keccak256(_DOMAIN_TYPEHASH + keccak256(name.encode()) + keccak256(version.encode())
                     + word(chain_id) + word(contract))


def typed_digest(domain: bytes, type_string: str, *fields) -> bytes:
    struct = keccak256(type_string.encode()) + b"".join(word(f) for f in fields)
    return keccak256(b"\x19\x01" + domain + keccak256(struct))


def random_nonce() -> bytes:
    return secrets.token_bytes(32)


if __name__ == "__main__":
    # self-check against known values
    assert keccak256(b"").hex() == "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470"
    assert keccak256(b"abc").hex() == "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45"
    k = bytes.fromhex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20")
    a = address(k)
    d = keccak256(b"hello")
    s = sign(k, d)
    assert recover(d, s).lower() == a.lower(), (recover(d, s), a)
    print("eth.py ok:", a)
