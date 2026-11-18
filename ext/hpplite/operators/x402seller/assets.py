"""The tokens a store or a relay takes (hpp-io/hpplite#357, #361): credit is in the first asset's atomic units; another
token converts at its unitsPerToken (credit units for one whole token: USDC.e and CHIP 1000000, HPP at $0.01 10000).
A token amount may pass 2^53 (HPP has 18 decimals). Shared by operators/store/hpplite_store.py and operators/relay/relay.py."""
import argparse
import re

SAFE = 2 ** 53 - 1
ADDR_RE = re.compile(r"^0x[0-9a-fA-F]{40}$")


def same(a, b):
    return bool(a) and bool(b) and a.lower() == b.lower()


def first_asset(address, name, version, decimals=6):
    """the asset credit is counted in"""
    return {"asset": address.lower(), "name": name, "version": version, "decimals": decimals, "unitsPerToken": 10 ** decimals,
            "exact": True, "upto": False}


def asset_info(assets, address):
    return next((x for x in assets if same(x["asset"], address)), None) if address else None


def to_units(asset, amount):
    """credit units for an amount of the token, rounded down"""
    return int(amount) * asset["unitsPerToken"] // 10 ** asset["decimals"]


def to_amount(asset, units):
    """the token amount that buys these credit units, rounded up"""
    return -(-int(units) * 10 ** asset["decimals"] // asset["unitsPerToken"])


def amt(n):
    """a token amount in JSON: a number up to 2^53 - 1, past it a decimal string"""
    return None if n is None else (int(n) if int(n) <= SAFE else str(int(n)))


def parse_asset(text):
    """--extra-asset ADDRESS,NAME,VERSION,DECIMALS,UNITS_PER_TOKEN[,SCHEMES]: SCHEMES the x402 schemes it is paid by, joined
    by '+' (exact: EIP-3009; upto: Permit2, #358); none: by permit only"""
    f = [x.strip() for x in text.split(",")]
    schemes = set(f[5].split("+")) if len(f) == 6 else set()
    if len(f) not in (5, 6) or not ADDR_RE.match(f[0]) or not schemes <= {"exact", "upto"}:
        raise argparse.ArgumentTypeError("an asset is ADDRESS,NAME,VERSION,DECIMALS,UNITS_PER_TOKEN[,exact|upto|exact+upto]")
    return {"asset": f[0].lower(), "name": f[1], "version": f[2], "decimals": int(f[3]), "unitsPerToken": int(f[4]),
            "exact": "exact" in schemes, "upto": "upto" in schemes}


def listed(assets):
    """/status's list of them"""
    return [{"asset": x["asset"], "name": x["name"], "version": x["version"], "decimals": x["decimals"],
             "unitsPerToken": str(x["unitsPerToken"]), "exact": x["exact"], "upto": x.get("upto", False)} for x in assets]


def other_offers(assets, units, upto_facilitator=None):
    """a 402's offers after the first asset's: {scheme, asset, amount, extra} for each other token with exact, then for each
    with upto (#358: the maximum is the whole amount, settled in full; the facilitator that may settle it in extra) when the
    facilitator takes upto (its address known)"""
    out = [{"scheme": "exact", "asset": x["asset"], "amount": to_amount(x, units), "extra": {"name": x["name"], "version": x["version"]}}
           for x in assets[1:] if x["exact"]]
    if upto_facilitator:
        out += [{"scheme": "upto", "asset": x["asset"], "amount": to_amount(x, units),
                 "extra": {"name": x["name"], "version": x["version"], "facilitatorAddress": upto_facilitator}}
                for x in assets if x.get("upto")]
    return out

