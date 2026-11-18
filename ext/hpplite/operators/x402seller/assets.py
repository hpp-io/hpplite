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
    return {"asset": address.lower(), "name": name, "version": version, "decimals": decimals, "unitsPerToken": 10 ** decimals, "exact": True}


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
    """--extra-asset ADDRESS,NAME,VERSION,DECIMALS,UNITS_PER_TOKEN[,exact]"""
    f = [x.strip() for x in text.split(",")]
    if len(f) not in (5, 6) or not ADDR_RE.match(f[0]) or (len(f) == 6 and f[5] != "exact"):
        raise argparse.ArgumentTypeError("an asset is ADDRESS,NAME,VERSION,DECIMALS,UNITS_PER_TOKEN[,exact]")
    return {"asset": f[0].lower(), "name": f[1], "version": f[2], "decimals": int(f[3]), "unitsPerToken": int(f[4]),
            "exact": len(f) == 6}


def listed(assets):
    """/status's list of them"""
    return [{"asset": x["asset"], "name": x["name"], "version": x["version"], "decimals": x["decimals"],
             "unitsPerToken": str(x["unitsPerToken"]), "exact": x["exact"]} for x in assets]


def other_offers(assets, units):
    """a 402's offers after the first asset's: (asset, amount, token extra) for each other token with exact"""
    return [(x["asset"], to_amount(x, units), {"name": x["name"], "version": x["version"]}) for x in assets[1:] if x["exact"]]
