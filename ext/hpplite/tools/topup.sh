#!/bin/sh
# Top up a node's wallet with USDC.e on HPP Sepolia by bridging USDC from
# Ethereum Sepolia through HPP's gateway (#11): what a node pays a store
# with over x402. Needs cast (foundry), Sepolia ETH for gas and the retryable
# ticket, and Sepolia USDC (https://faucet.circle.com) in the wallet of the
# key. The deposit lands on HPP in a few minutes.
#
#   sh tools/topup.sh [--dry-run] [--key-file FILE | --env ENVFILE] [--to 0x...] AMOUNT_USDC
#
# The key is HPPLITE_PRIVATE_KEY from the environment, from --env (a .env
# file, default ./.env) or from --key-file. --to sends the USDC.e to another
# wallet on HPP (a node's) instead of the key's own. --dry-run prints the
# two transactions (approve, outboundTransfer) with the fees it computed
# and sends nothing. Addresses are HPP Sepolia's (chain 181228).
set -eu

L1=${HPPLITE_TOPUP_L1_RPC:-https://ethereum-sepolia-rpc.publicnode.com}
L2=${HPPLITE_L1_RPC:-https://sepolia.hpp.io}
USDC_L1=0x1c7D4B196Cb0C7B01d743Fbc6116a902379C7238       # Circle's USDC on Ethereum Sepolia
ROUTER=0x8deBBE23AAE151C49609A46ad2b14590e3692F21        # HPP Sepolia L1 gateway router
GATEWAY=0xE6df1ae37400BF27379EBfC13B0962267a17652c       # ...its ERC-20 gateway (the approve's spender)
INBOX=0xAAD45a7bF65b43E56767CdE3Ab84A5433c714Afc         # ...inbox: the retryable ticket's submission fee
USDC_L2=0x401eCb1D350407f13ba348573E5630B83638E30D       # USDC.e on HPP Sepolia

dry=0; envfile=./.env; keyfile=""; to=""; amount=""
while [ $# -gt 0 ]; do
  case "$1" in
    --dry-run) dry=1 ;;
    --env) envfile=$2; shift ;;
    --key-file) keyfile=$2; shift ;;
    --to) to=$2; shift ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) amount=$1 ;;
  esac
  shift
done
[ -n "$amount" ] || { echo "usage: sh tools/topup.sh [--dry-run] [--key-file FILE | --env ENVFILE] [--to 0x...] AMOUNT_USDC" >&2; exit 2; }
command -v cast >/dev/null || { echo "cast (foundry) is not installed" >&2; exit 2; }

key=${HPPLITE_PRIVATE_KEY:-}
[ -n "$keyfile" ] && key=$(tr -d ' \r\n' < "$keyfile")
[ -z "$key" ] && [ -f "$envfile" ] && key=$(sed -n 's/^HPPLITE_PRIVATE_KEY=//p' "$envfile" | head -1)
[ -n "$key" ] || { echo "no key: HPPLITE_PRIVATE_KEY, --env or --key-file" >&2; exit 2; }
wallet=$(cast wallet address "$key")
[ -n "$to" ] || to=$wallet
units=$(python3 -c "import sys; from decimal import Decimal; print(int(Decimal(sys.argv[1]) * 1000000))" "$amount")

echo "wallet $wallet; $amount USDC ($units units) to $to on HPP Sepolia"
echo "L1 USDC balance: $(cast call $USDC_L1 'balanceOf(address)(uint256)' "$wallet" --rpc-url "$L1" | awk '{print $1}') units; L1 ETH: $(cast from-wei "$(cast balance "$wallet" --rpc-url "$L1")")"
echo "HPP USDC.e balance now: $(cast call $USDC_L2 'balanceOf(address)(uint256)' "$to" --rpc-url "$L2" | awk '{print $1}') units"

# the retryable ticket: submission fee from the inbox at twice the base fee, three times that as the cap;
# L2 gas at ten times the current price so the ticket is not stranded
l1base=$(cast base-fee --rpc-url "$L1" 2>/dev/null || cast gas-price --rpc-url "$L1")
sub=$(cast call $INBOX 'calculateRetryableSubmissionFee(uint256,uint256)(uint256)' 1200 $((l1base * 2)) --rpc-url "$L1" | awk '{print $1}')
maxSub=$((sub * 3)); maxGas=1500000
l2price=$(cast gas-price --rpc-url "$L2"); gasBid=$((l2price * 10))
value=$((maxSub + maxGas * gasBid))
data=$(cast abi-encode 'f(uint256,bytes)' "$maxSub" 0x)
echo "fees: L1 base $l1base wei; submission $sub, sending $maxSub; L2 gas $gasBid wei x $maxGas; value $value wei ($(cast from-wei $value) ETH)"

if [ "$dry" = 1 ]; then
  echo "--- dry run: would send"
  echo "cast send $USDC_L1 'approve(address,uint256)' $GATEWAY $units --rpc-url $L1"
  echo "cast send $ROUTER 'outboundTransfer(address,address,uint256,uint256,uint256,bytes)' $USDC_L1 $to $units $maxGas $gasBid $data --value $value --rpc-url $L1"
  exit 0
fi
echo "--- approve"
cast send $USDC_L1 'approve(address,uint256)' $GATEWAY "$units" --private-key "$key" --rpc-url "$L1" --json \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print('status', r.get('status'), 'tx', r.get('transactionHash'))"
echo "--- deposit"
cast send $ROUTER 'outboundTransfer(address,address,uint256,uint256,uint256,bytes)' $USDC_L1 "$to" "$units" $maxGas "$gasBid" "$data" --value "$value" --private-key "$key" --rpc-url "$L1" --json \
  | python3 -c "import sys,json; r=json.load(sys.stdin); print('status', r.get('status'), 'tx', r.get('transactionHash'), 'gasUsed', int(r.get('gasUsed','0x0'),16))"
echo "the USDC.e lands at $to on HPP Sepolia in a few minutes: cast call $USDC_L2 'balanceOf(address)(uint256)' $to --rpc-url $L2"
