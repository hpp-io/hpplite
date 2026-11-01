// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title EIP712Sig
 * @notice The signed forms of the rollup's calls (#33, #126): the sequencer
 *         wallet signs an EIP-712 message, anyone submits it and pays the gas.
 */
library EIP712Sig {
    bytes32 internal constant DOMAIN_TYPEHASH =
        keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");

    function domain(string memory name, string memory version, address verifyingContract) internal view returns (bytes32) {
        return keccak256(abi.encode(DOMAIN_TYPEHASH, keccak256(bytes(name)), keccak256(bytes(version)), block.chainid, verifyingContract));
    }

    function digest(bytes32 domainSeparator, bytes32 structHash) internal pure returns (bytes32) {
        return keccak256(abi.encodePacked("\x19\x01", domainSeparator, structHash));
    }

    /// The signer of `d`, or address(0) for a malformed signature
    function recover(bytes32 d, bytes calldata sig) internal pure returns (address) {
        if (sig.length != 65) return address(0);
        bytes32 r = bytes32(sig[0:32]);
        bytes32 s = bytes32(sig[32:64]);
        uint8 v = uint8(sig[64]);
        if (v < 27) v += 27;
        if (v != 27 && v != 28) return address(0);
        // the high-s half of the curve is a second valid signature for the same message
        if (uint256(s) > 0x7FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFF5D576E7357A4501DDFE92F46681B20A0) return address(0);
        return ecrecover(d, v, r, s);
    }
}
