// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Test.sol";
import "../src/test/MockUSDC.sol";

/// MockUSDC's EIP-2612 permit, as USDC.e has it (#212): what hpplite-store/2's pulls are funded by
contract MockUSDCPermitTest is Test {
    MockUSDC token;
    uint256 ownerKey = 0xA11CE;
    address owner;
    address spender = address(0xB0B);
    bytes32 constant PERMIT_TYPEHASH =
        keccak256("Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)");

    function setUp() public {
        token = new MockUSDC(1_000_000_000);
        owner = vm.addr(ownerKey);
        token.transfer(owner, 50_000_000);
    }

    function sign(uint256 key, address o, uint256 value, uint256 nonce, uint256 deadline)
        internal view returns (uint8 v, bytes32 r, bytes32 s)
    {
        bytes32 digest = keccak256(abi.encodePacked("\x19\x01", token.DOMAIN_SEPARATOR(),
            keccak256(abi.encode(PERMIT_TYPEHASH, o, spender, value, nonce, deadline))));
        (v, r, s) = vm.sign(key, digest);
    }

    function test_PermitSetsAllowanceAndPull() public {
        assertEq(token.PERMIT_TYPEHASH(), PERMIT_TYPEHASH);
        (uint8 v, bytes32 r, bytes32 s) = sign(ownerKey, owner, 20_000_000, 0, block.timestamp + 1 days);
        token.permit(owner, spender, 20_000_000, block.timestamp + 1 days, v, r, s);
        assertEq(token.allowance(owner, spender), 20_000_000);
        assertEq(token.nonces(owner), 1);
        vm.prank(spender);
        token.transferFrom(owner, address(0xCAFE), 3_000_000);
        assertEq(token.balanceOf(address(0xCAFE)), 3_000_000);
        assertEq(token.allowance(owner, spender), 17_000_000);
    }

    function test_PermitReusedNonceReverts() public {
        (uint8 v, bytes32 r, bytes32 s) = sign(ownerKey, owner, 1, 0, block.timestamp + 1 days);
        token.permit(owner, spender, 1, block.timestamp + 1 days, v, r, s);
        vm.expectRevert();
        token.permit(owner, spender, 1, block.timestamp + 1 days, v, r, s);
    }

    function test_PermitPastDeadlineReverts() public {
        vm.warp(1_000_000);
        (uint8 v, bytes32 r, bytes32 s) = sign(ownerKey, owner, 1, 0, block.timestamp - 1);
        vm.expectRevert();
        token.permit(owner, spender, 1, block.timestamp - 1, v, r, s);
    }

    function test_PermitWrongSignerReverts() public {
        (uint8 v, bytes32 r, bytes32 s) = sign(0xBADBAD, owner, 1, 0, block.timestamp + 1 days);
        vm.expectRevert();
        token.permit(owner, spender, 1, block.timestamp + 1 days, v, r, s);
    }
}
