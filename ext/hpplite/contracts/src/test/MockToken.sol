// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title MockToken
 * @notice MockUSDC with its name, symbol and EIP-712 version given at deploy (somaro#128): a stand-in for CHIP
 *         ("House Chip", "CHIP", "1") or any other token an operator prices in, so a test sees whatever assumes
 *         USDC.e's domain. Otherwise as MockUSDC: an ERC-20 with EIP-3009
 *         transferWithAuthorization, which is what x402's `exact` scheme
 *         settles with, and receiveWithAuthorization, which batch-settlement's
 *         deposit collector pulls a channel's deposit with (#135), and EIP-2612
 *         permit, which hpplite-store/2's pulls are funded by (#212). Six decimals; the deployer gets the supply.
 *         Not for any chain that matters. The EIP-712 domain is the real
 *         token's: name "Bridged USDC", version "2" (its symbol is USDC.e;
 *         HPP's facilitator checks the name against the token, #11).
 */
contract MockToken {
    string public name;
    string public symbol;
    string private domainVersion;
    uint8 public constant decimals = 6;
    uint256 public totalSupply;
    mapping(address => uint256) public balanceOf;
    mapping(address => mapping(address => uint256)) public allowance;
    mapping(address => mapping(bytes32 => bool)) public authorizationState;
    mapping(address => uint256) public nonces;

    bytes32 public constant TRANSFER_WITH_AUTHORIZATION_TYPEHASH =
        keccak256("TransferWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)");
    bytes32 public constant RECEIVE_WITH_AUTHORIZATION_TYPEHASH =
        keccak256("ReceiveWithAuthorization(address from,address to,uint256 value,uint256 validAfter,uint256 validBefore,bytes32 nonce)");
    bytes32 public constant PERMIT_TYPEHASH =
        keccak256("Permit(address owner,address spender,uint256 value,uint256 nonce,uint256 deadline)");
    bytes32 private constant DOMAIN_TYPEHASH =
        keccak256("EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");

    event Transfer(address indexed from, address indexed to, uint256 value);
    event Approval(address indexed owner, address indexed spender, uint256 value);
    event AuthorizationUsed(address indexed authorizer, bytes32 indexed nonce);

    constructor(string memory name_, string memory symbol_, string memory version_, uint256 supply) {
        name = name_;
        symbol = symbol_;
        domainVersion = version_;
        totalSupply = supply;
        balanceOf[msg.sender] = supply;
        emit Transfer(address(0), msg.sender, supply);
    }

    function DOMAIN_SEPARATOR() public view returns (bytes32) {
        return keccak256(abi.encode(DOMAIN_TYPEHASH, keccak256(bytes(name)), keccak256(bytes(domainVersion)), block.chainid, address(this)));
    }

    function transfer(address to, uint256 value) external returns (bool) {
        _move(msg.sender, to, value);
        return true;
    }

    function approve(address spender, uint256 value) external returns (bool) {
        allowance[msg.sender][spender] = value;
        emit Approval(msg.sender, spender, value);
        return true;
    }

    function transferFrom(address from, address to, uint256 value) external returns (bool) {
        require(allowance[from][msg.sender] >= value, "allowance");
        allowance[from][msg.sender] -= value;
        _move(from, to, value);
        return true;
    }

    /// EIP-2612: `owner` signed an allowance for `spender`, anyone submits it, its nonce once
    function permit(address owner, address spender, uint256 value, uint256 deadline, uint8 v, bytes32 r, bytes32 s) external {
        require(block.timestamp <= deadline, "permit expired");
        bytes32 digest = keccak256(abi.encodePacked("\x19\x01", DOMAIN_SEPARATOR(),
            keccak256(abi.encode(PERMIT_TYPEHASH, owner, spender, value, nonces[owner]++, deadline))));
        address signer = ecrecover(digest, v, r, s);
        require(signer != address(0) && signer == owner, "invalid signature");
        allowance[owner][spender] = value;
        emit Approval(owner, spender, value);
    }

    /// EIP-3009: `from` signed it, anyone submits it, the nonce is used once
    function transferWithAuthorization(
        address from, address to, uint256 value, uint256 validAfter, uint256 validBefore, bytes32 nonce,
        uint8 v, bytes32 r, bytes32 s
    ) external {
        _authorized(TRANSFER_WITH_AUTHORIZATION_TYPEHASH, from, to, value, validAfter, validBefore, nonce, v, r, s);
    }

    /// EIP-3009's receive form: as above, but only `to` may submit it (no front-running into another call)
    function receiveWithAuthorization(
        address from, address to, uint256 value, uint256 validAfter, uint256 validBefore, bytes32 nonce,
        bytes memory signature
    ) external {
        require(msg.sender == to, "caller must be the payee");
        require(signature.length == 65, "signature length");
        bytes32 r; bytes32 s; uint8 v;
        assembly { r := mload(add(signature, 32)) s := mload(add(signature, 64)) v := byte(0, mload(add(signature, 96))) }
        _authorized(RECEIVE_WITH_AUTHORIZATION_TYPEHASH, from, to, value, validAfter, validBefore, nonce, v, r, s);
    }

    function _authorized(bytes32 typehash, address from, address to, uint256 value, uint256 validAfter, uint256 validBefore,
                         bytes32 nonce, uint8 v, bytes32 r, bytes32 s) internal {
        require(block.timestamp > validAfter, "not yet valid");
        require(block.timestamp < validBefore, "expired");
        require(!authorizationState[from][nonce], "nonce used");
        bytes32 digest = keccak256(abi.encodePacked("\x19\x01", DOMAIN_SEPARATOR(),
            keccak256(abi.encode(typehash, from, to, value, validAfter, validBefore, nonce))));
        address signer = ecrecover(digest, v, r, s);
        require(signer != address(0) && signer == from, "invalid signature");
        authorizationState[from][nonce] = true;
        emit AuthorizationUsed(from, nonce);
        _move(from, to, value);
    }

    function _move(address from, address to, uint256 value) internal {
        require(balanceOf[from] >= value, "balance");
        balanceOf[from] -= value;
        balanceOf[to] += value;
        emit Transfer(from, to, value);
    }
}
