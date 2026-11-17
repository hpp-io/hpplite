// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/// A smart wallet for the tests (hpp-io/hpplite#356): one owner key, and ERC-1271. It accepts a signature that is a
/// mode byte 0x00 then the owner's 65-byte signature over the hash - 66 bytes, as a smart wallet's are seldom 65 - so a
/// store that recovers 65-byte signatures only refuses it, and one that asks isValidSignature takes it
contract MockWallet1271 {
    address public immutable owner;

    constructor(address owner_) {
        owner = owner_;
    }

    function isValidSignature(bytes32 hash, bytes calldata signature) external view returns (bytes4) {
        if (signature.length != 66 || signature[0] != 0x00) return 0xffffffff;
        bytes32 r = bytes32(signature[1:33]);
        bytes32 s = bytes32(signature[33:65]);
        uint8 v = uint8(signature[65]);
        address signer = ecrecover(hash, v, r, s);
        return signer != address(0) && signer == owner ? bytes4(0x1626ba7e) : bytes4(0xffffffff);
    }
}
