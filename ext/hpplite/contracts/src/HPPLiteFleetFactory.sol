// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./HPPLiteFleet.sol";
import "./EIP712Sig.sol";

/**
 * @title HPPLiteFleetFactory
 * @notice VERSION 11 (#297): creates shared rollups (HPPLiteFleet), beside VERSION 10's factory of dedicated rollups,
 *         which stays as it is. A fleet's owner creates it, or signs CreateFleet for a relay to submit.
 */
contract HPPLiteFleetFactory {
    using EIP712Sig for bytes32;

    uint256 public constant VERSION = 11;
    bytes32 public constant CREATE_FLEET_TYPEHASH =
        keccak256("CreateFleet(address owner,address anchor,uint64 leaseDuration,uint256 nonce,uint256 deadline)");

    event FleetCreated(address indexed fleet, address indexed owner, address anchor, uint64 leaseDuration);

    address[] public fleets;
    mapping(address => address[]) public fleetsByOwner;
    mapping(address => bool) public isFleet;
    mapping(address => uint256) public createNonce;      // owner => spent by each createFleetFor

    function domainSeparator() public view returns (bytes32) {
        return EIP712Sig.domain("HPPLiteFleetFactory", "11", address(this));
    }

    function createDigest(address owner, address anchor, uint64 leaseDuration, uint256 nonce, uint256 deadline) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(CREATE_FLEET_TYPEHASH, owner, anchor, leaseDuration, nonce, deadline)));
    }

    function createFleet(address anchor, uint64 leaseDuration) external returns (address) {
        return _create(msg.sender, anchor, leaseDuration);
    }

    function createFleetFor(address owner, address anchor, uint64 leaseDuration, uint256 nonce, uint256 deadline, bytes calldata sig)
        external returns (address)
    {
        require(block.timestamp <= deadline, "Expired");
        require(nonce == createNonce[owner], "Bad nonce");
        require(createDigest(owner, anchor, leaseDuration, nonce, deadline).recover(sig) == owner, "Not the owner's signature");
        createNonce[owner]++;
        return _create(owner, anchor, leaseDuration);
    }

    function fleetCount(address owner) external view returns (uint256) {
        return fleetsByOwner[owner].length;
    }

    function _create(address owner, address anchor, uint64 leaseDuration) internal returns (address fleet) {
        require(owner != address(0) && anchor != address(0), "No owner or anchor");
        fleet = address(new HPPLiteFleet(owner, anchor, leaseDuration));
        fleets.push(fleet);
        fleetsByOwner[owner].push(fleet);
        isFleet[fleet] = true;
        emit FleetCreated(fleet, owner, anchor, leaseDuration);
    }
}
