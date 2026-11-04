# x402 batch-settlement contracts (#135)

Runtime bytecode of x402's `x402BatchSettlement` and `ERC3009DepositCollector`,
read with `eth_getCode` from HPP Sepolia (eip155:181228) at block 449844 on
2026-11-04. They sit at the same canonical CREATE2 addresses on every chain,
so `install_x402_escrow.sh` puts them at those addresses on anvil with
`anvil_setCode`: the tests run the code HPP's facilitator settles against,
not a re-implementation. Neither has constructor state beyond immutables:
the collector's escrow address is the canonical one, and OpenZeppelin's EIP712
in the escrow rebuilds its domain separator for anvil's chain id.

| contract | address | bytes | keccak256 |
|---|---|---|---|
| x402BatchSettlement | 0x4020074e9dF2ce1deE5A9C1b5c3f541D02a10003 | 11175 | 0x20fada8fa7bfd628b38b7872f5717a6539c87ea35b2f60cd3a07f70b41ce3a22 |
| ERC3009DepositCollector | 0x4020806089470a89826cB9fB1f4059150b550004 | 1150 | 0x0c2cfe8840e964588598801e8d109593fdc57e16d85f214653139c79186d1f3b |

Source: github.com/coinbase/x402, contracts/evm/src (MIT).
