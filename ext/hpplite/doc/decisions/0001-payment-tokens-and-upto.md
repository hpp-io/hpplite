# 0001. Payment tokens: CHIP now, HPP through upto later

2026-11-12. Agreed with somaro (its decision 0021, the user's answers). Not built yet: CHIP on mainnet
comes first, HPP waits for the vault's address.

## Context

HPP's x402 facilitators settle three tokens: USDC.e and CHIP (6 decimals; `exact`, `upto` and
`batch-settlement`; EIP-3009 and EIP-2612) and HPP (18 decimals; `upto` only, through Permit2). Every
operator prices in its asset's base units, read as $0.000001 a unit: the relay's call is gas × fee ×
ETH/USD × 1e6, and the store charges `PRICE_PER_WRITE` and `PRICE_PER_MIB_MONTH` and pulls past
`PULL_THRESHOLD`. The node's caps (`HPPLITE_RELAY_CAP`, `HPPLITE_STORE_CAP`, the default store permit)
are in the same units. The store's daily caps (writes, bytes) and the relay's per-database CapDb (calls)
are not money.

## Decision

1. **CHIP is $1, with no feed.** The asset is already a setting (`ASSET`, `ASSET_NAME`, `ASSET_VERSION`
   in the operators; the node signs whatever token and EIP-712 domain the operator's offer and `/status`
   name), and the 6-decimal, $0.000001-a-unit pricing holds as it is. Shown on HPP Sepolia on
   2026-11-12: a paid relay call, a store top-up and a fleet join, all in CHIP (the `sepolia-chip`
   operators), and on anvil by scenario 55.
2. **The token setting carries its scheme.** A token is `{address, decimals, EIP-712 name and version,
   scheme: exact | upto}`. Nothing new is wired to EIP-3009 alone, since `upto` is the one scheme all three
   tokens share. `exact` stays for USDC.e and CHIP until `upto` is in.
3. **Prices in USD, a token amount only at settlement.** Prices are kept in USD micro-units, and a quote
   or a pull converts them: `amount = ceil(usd × 10^decimals / 1e6 / price)`. USDC.e and CHIP are at $1;
   HPP is at the price reader's.
4. **What upto carries:**
   - **Relay calls**, the lease (claim, renew) and the epoch among them: per call, the 402 quotes the
     maximum (the estimate plus headroom, the gas limit sent) and settles what the receipt used, which is
     the at-cost rule (#294) done by the scheme.
   - **Store bytes and writes**: authorized once and settled for use, as today. For USDC.e and CHIP that
     is an EIP-2612 permit to the store's spender with pulls of what the bucket owes. For HPP it is a
     gasless EIP-2612 permit of HPP to Permit2 (once), then a Permit2 allowance (amount, expiration) to
     the spender, with pulls as Permit2 `transferFrom`. The permit's value is the cap and its deadline
     the period.
5. **One store, a token list.** Each funder's account is in one token (its permit's). Credit, debt and
   pulls are kept in USD units and converted at pull time; the relay's sponsor accounts work the same.
   The Sepolia CHIP pair is a separate deployment only to keep staging steady.
6. **One price reader, HPP only.** CoinGecko (`aergo-2`) and KuCoin's public HPP-USDT ticker are read,
   and the mean is used when they agree within 3% and both are under 5 minutes old. Otherwise HPP is
   refused for new quotes and pulls ("stale or disputed") while the other tokens go on. There is **no
   last-good fallback for HPP**: an old price for a volatile token is a loss (unlike ETH for the relay's
   gas, #47). It is one Worker or object that the store and the relay both ask, so they never quote two
   HPP prices; its CoinGecko key is a Cloudflare secret.
7. **HPP is kept, not converted.** `payTo` for HPP is the HPP team's vault address itself (`HPP_VAULT`, a
   setting until the team gives it): x402 settlement and Permit2 `transferFrom` both deliver straight
   there, so there is no forwarding step and no hot wallet holding HPP. hppyswap plays no part.

## Consequences

The work, in order, when it is started: the token list in the operators; USD-unit accounting with
conversion at quote and pull; the shared price reader; Permit2 typed data in the node (new) and the
operators; `upto` per call in the relay. The node's caps move to USD units. Mainnet launches in CHIP first
(somaro's MAINNET.md); HPP follows once the vault exists.
