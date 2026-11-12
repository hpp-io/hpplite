#!/bin/sh
# Real-contract tests on a local chain (issue #47): boot anvil, deploy the
# factory with forge, run test_anvil (real-L1 build) against it, shut down.
#   tests/l1/anvil_test.sh <path/to/test_anvil>
# Exit 77 = skipped (anvil/forge/cast not installed). No network, no funds.
# HPPLITE_ANVIL_FORK=https://mainnet.hpp.io: the same on a fork of that chain with its real CHIP (#346); the
# network is read, nothing is sent to it. HPPLITE_RELAY_ASSET=chip runs the relay's fleet selftest in CHIP.
set -eu
bin=$1
here=$(cd "$(dirname "$0")" && pwd)
contracts=$(cd "$here/../../contracts" && pwd)
for t in anvil forge cast; do
    command -v $t >/dev/null 2>&1 || { echo "anvil_test: $t not found (install foundry); skipped"; exit 77; }
done
# a port free now, from the OS: one derived from the pid may be another
# program's, even another anvil's, which would answer for this one (#177)
port=${HPPLITE_ANVIL_PORT:-$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')}
rpc="http://127.0.0.1:$port"
# anvil's ten default accounts (public test keys, 10000 ETH each)
key0=0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80
key1=0x59c6995e998f97a5a0044966f0945389dc9e86dae88c7a8412f4603b6b78690d
key2=0x5de4111afa1a4b94908f83103eb1f1706367c2e68ca870fc3fb9a804cdab365a
key3=0x7c852118294e51e653712a81e05800f419141751be58f605c371e15141b007a6
key4=0x47e179ec197488593b187f80a00eb0da91f1b9d0b13f8733639f19c30a34926a
key5=0x8b3a350cf5c34c9194ca85829a2df0ec3153be0318b5e2d3348e872092edffba
key6=0x92db14e403b83dfe3df233f83dfa3a0d7096f21ca9b0d6d6b8d88b2b4ec1564e
key7=0x4bbbf85ce3377467afe5d46f804f221813b2bb87f24d81f60f1fcdbf7cbf4356
key8=0xdbda1821b80551c9d65939329250298aa3472ba22feea921c0cf5d620ea67b97
key9=0x2a871d0798f97d79848a013d4936a73bf4cc922c825d33c1cf7073dff6d409c6
log=${TMPDIR:-/tmp}/hpplite-anvil-$$.log
# HPP Sepolia's block gas limit (2^50), not anvil's default 30M: a full
# 128 KB batch in contract storage needs ~90M gas (#51)
# HPPLITE_ANVIL_ARGS: extra anvil flags (the soak passes --prune-history: an
# hour of blocks otherwise fills the runner's disk with state snapshots)
# shellcheck disable=SC2086
# HPPLITE_ANVIL_FORK=<rpc>: anvil forks that chain (#346, somaro#140: HPP Mainnet's real CHIP in place of the stand-in);
# the chain id stays 31337, the one every scenario signs for
fork=
[ -n "${HPPLITE_ANVIL_FORK:-}" ] && fork="--fork-url $HPPLITE_ANVIL_FORK"
anvil --port "$port" --chain-id 31337 --silent --block-time 1 --gas-limit 1125899906842624 $fork ${HPPLITE_ANVIL_ARGS:-} >"$log" 2>&1 &
pid=$!
trap 'kill -CONT $pid 2>/dev/null; kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$log"' EXIT INT TERM
for i in $(seq 1 50); do kill -0 $pid 2>/dev/null || break; cast chain-id --rpc-url "$rpc" >/dev/null 2>&1 && break; sleep 0.2; done
kill -0 $pid 2>/dev/null && cast chain-id --rpc-url "$rpc" >/dev/null 2>&1 || { echo "anvil did not start"; cat "$log"; exit 1; }
(cd "$contracts" && HPPLITE_PRIVATE_KEY=$key0 forge script script/DeployFactory.s.sol --rpc-url "$rpc" --broadcast >"$log.forge" 2>&1) \
    || { echo "factory deployment failed"; cat "$log.forge"; exit 1; }
# the console line's format changes between forge versions; the broadcast record does not
factory=$(sed -n 's/.*"contractAddress": *"\(0x[0-9a-fA-F]\{40\}\)".*/\1/p' "$contracts/broadcast/DeployFactory.s.sol/31337/run-latest.json" | head -1)
rm -f "$log.forge"
[ -n "$factory" ] || { echo "factory address not found in broadcast record"; exit 1; }
# forge script prints the simulated address even when the broadcast reverted (e.g. over the 24 KB size limit)
[ "$(cast code "$factory" --rpc-url "$rpc" 2>/dev/null | wc -c)" -gt 10 ] || { echo "factory at $factory has no code: deployment reverted (forge build --sizes)"; exit 1; }
echo "anvil on $rpc, factory $factory"
# a stand-in USDC.e with EIP-3009 for the pin service's x402 payments (#125); key0 holds the supply
usdc=$(cd "$contracts" && forge create --rpc-url "$rpc" --private-key "$key0" --broadcast --json src/test/MockUSDC.sol:MockUSDC --constructor-args 1000000000000000 2>/dev/null | sed -n 's/.*"deployedTo": *"\(0x[0-9a-fA-F]\{40\}\)".*/\1/p' | head -1)
[ -n "$usdc" ] && echo "MockUSDC $usdc" || echo "MockUSDC not deployed: the paid store scenarios will be skipped"
# a stand-in for CHIP (somaro#128): another token, another EIP-712 domain ("House Chip", "1")
if [ -n "$fork" ]; then
    # the real CHIP (HPP Mainnet), its balances a mapping at slot 0: key0 given the stand-in's supply on the fork
    chip=${HPPLITE_ANVIL_FORK_CHIP:-0xEDFC8E6658561C97A3FFc966923d16C1c626083A}
    addr0=$(cast wallet address "$key0")
    cast rpc --rpc-url "$rpc" anvil_setStorageAt "$chip" "$(cast index address "$addr0" 0)" "$(cast to-uint256 1000000000000000)" >/dev/null
    [ "$(cast call "$chip" 'balanceOf(address)(uint256)' "$addr0" --rpc-url "$rpc" | cut -d' ' -f1)" = 1000000000000000 ] \
        || { echo "the fork's CHIP at $chip: key0's balance not set (its balances not at slot 0?)"; exit 1; }
    echo "fork of $HPPLITE_ANVIL_FORK at block $(cast block-number --rpc-url "$rpc"), CHIP $chip ($(cast call "$chip" 'name()(string)' --rpc-url "$rpc"))"
else
    chip=$(cd "$contracts" && forge create --rpc-url "$rpc" --private-key "$key0" --broadcast --json src/test/MockToken.sol:MockToken --constructor-args "House Chip" CHIP 1 1000000000000000 2>/dev/null | sed -n 's/.*"deployedTo": *"\(0x[0-9a-fA-F]\{40\}\)".*/\1/p' | head -1)
fi
[ -n "$fork" ] || { [ -n "$chip" ] && echo "MockToken (CHIP) $chip" || echo "MockToken (CHIP) not deployed: the CHIP scenario will be skipped"; }
# the x402 facilitator the services pay through on this chain (#183), with a key of its own that no test uses
fkey=0x0fac11177a70f0fac11177a70f0fac11177a70f0fac11177a70f0fac11177a70
faddr=$(cast wallet address "$fkey")
cast rpc --rpc-url "$rpc" anvil_setBalance "$faddr" 0x21e19e0c9bab2400000 >/dev/null
fport=$(python3 -I -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
python3 -I "$here/../../operators/conformance/facilitator_mock.py" --port "$fport" --rpc "$rpc" --key "$fkey" >"$log.facilitator" 2>&1 &
fpid=$!
trap 'kill $fpid 2>/dev/null; kill -CONT $pid 2>/dev/null; kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -f "$log" "$log.facilitator"' EXIT INT TERM
for i in $(seq 1 50); do kill -0 $fpid 2>/dev/null || break; curl -s -m 2 "http://127.0.0.1:$fport/supported" 2>/dev/null | grep -q kinds && break; sleep 0.2; done
curl -s -m 2 "http://127.0.0.1:$fport/supported" 2>/dev/null | grep -q kinds || { echo "the mock facilitator did not start"; cat "$log.facilitator"; exit 1; }
export HPPLITE_ANVIL_FACILITATOR="http://127.0.0.1:$fport" HPPLITE_ANVIL_FACILITATOR_ADDR=$faddr
export HPPLITE_ANVIL_USDC=$usdc HPPLITE_ANVIL_CHIP=$chip HPPLITE_PIN_SERVICE="$here/../../operators/store"
# the faulty-endpoint proxy (#49), started by the suite itself in front of anvil
HPPLITE_VERIFY_BIN="$(dirname "$bin")/hpplite-verify" HPPLITE_RPC_PROXY="$here/rpc_proxy.py" HPPLITE_ANVIL_RPC=$rpc HPPLITE_ANVIL_FACTORY=$factory HPPLITE_ANVIL_KEY0=$key0 HPPLITE_ANVIL_KEY1=$key1 HPPLITE_ANVIL_KEY2=$key2 HPPLITE_ANVIL_KEY3=$key3 HPPLITE_ANVIL_KEY4=$key4 HPPLITE_ANVIL_KEY5=$key5 HPPLITE_ANVIL_KEY6=$key6 HPPLITE_ANVIL_KEY7=$key7 HPPLITE_ANVIL_KEY8=$key8 HPPLITE_ANVIL_KEY9=$key9 HPPLITE_ANVIL_PID=$pid "$bin"
