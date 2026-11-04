#!/bin/sh
# Put x402's batch-settlement escrow and its ERC-3009 deposit collector on an
# anvil chain, at their canonical addresses, from the bytecode HPP Sepolia runs
# (fixtures/README.md). Usage: install_x402_escrow.sh <rpc>
set -e
rpc=$1
dir=$(cd "$(dirname "$0")/fixtures" && pwd)
cast rpc --rpc-url "$rpc" anvil_setCode 0x4020074e9dF2ce1deE5A9C1b5c3f541D02a10003 "$(cat "$dir/x402BatchSettlement.hex")" >/dev/null
cast rpc --rpc-url "$rpc" anvil_setCode 0x4020806089470a89826cB9fB1f4059150b550004 "$(cat "$dir/ERC3009DepositCollector.hex")" >/dev/null
# the escrow answers for its domain: name "x402 Batch Settlement", version "1"
cast call --rpc-url "$rpc" 0x4020074e9dF2ce1deE5A9C1b5c3f541D02a10003 "MIN_WITHDRAW_DELAY()(uint40)" >/dev/null
