// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./EIP712Sig.sol";

/**
 * @title HPPLiteFleet
 * @notice A shared rollup (hpplite-fleet/1, doc/fleet-protocol.md, #297): many databases under one contract.
 *         Per database: its owner, its sequencer, the lease and its epoch (the fencing token a store checks),
 *         `epochSince` (the root sequence from which the current epoch holds), retirement and where its history
 *         began (a dedicated rollup it migrated from). Per fleet: one root per anchor interval over the heads
 *         that moved, posted by the fleet's anchor key. The root is not checked per database here - a verifier
 *         checks a database's leaf and path against it, and its head's signature against the database's
 *         sequencer and epoch.
 */
contract HPPLiteFleet {
    using EIP712Sig for bytes32;

    uint256 public constant VERSION = 11;

    bytes32 public constant HEAD_TYPEHASH = keccak256(
        "Head(bytes32 dbId,bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 chainHashAfter,bytes32 stateRoot,bytes32 manifestDigest,uint256 epoch)");
    bytes32 public constant ADMIT_TYPEHASH = keccak256("Admit(bytes32 dbId,address owner,address sequencer,uint256 issuedAt)");
    bytes32 public constant RETIRE_TYPEHASH = keccak256("Retire(bytes32 dbId,uint256 issuedAt)");
    bytes32 public constant ROOT_TYPEHASH = keccak256("Root(uint256 seq,bytes32 root,bytes32 manifestDigest,uint256 deadline)");
    /// the database's sequencer takes or renews its lease (a relay submits it)
    bytes32 public constant CLAIM_TYPEHASH = keccak256("Claim(bytes32 dbId,bytes32 instanceId,uint256 nonce,uint256 deadline)");

    struct Db {
        address owner;
        address sequencer;
        bytes32 instance;       // the lease's holder
        uint64 until;           // the lease's end
        uint64 epoch;           // moves at every new holder, forceExpire and rotation: the store refuses older names
        uint64 epochSince;      // the root sequence from which `epoch` holds
        uint64 admittedAt;      // the Admit's issuedAt (a retired dbId may be admitted again, later)
        bool retired;
        uint256 claimNonce;
        address migratedFrom;   // a dedicated rollup this database's history began on, or 0
        uint256 genesisHeight;  // its last height there
        bytes32 genesisChain;   // the chain hash after it
        bytes32 genesisRoot;    // the state root after it
        address promotedTo;     // a dedicated rollup it left for, or 0
    }

    address public owner;
    address public admitter;
    address public anchor;
    uint64 public leaseDuration;
    uint256 public rootSeq;
    /// keccak256(abi.encode(root, manifestDigest)) per sequence: one slot a root; both are in RootPosted in full
    mapping(uint256 => bytes32) public rootHash;
    mapping(bytes32 => Db) private _dbs;

    event Admitted(bytes32 indexed dbId, address indexed owner, address sequencer, address migratedFrom);
    event Claimed(bytes32 indexed dbId, bytes32 instance, uint64 until);
    event EpochMoved(bytes32 indexed dbId, uint64 epoch, uint64 sinceSeq);
    event RootPosted(uint256 indexed seq, bytes32 root, bytes32 manifestDigest);
    event Retired(bytes32 indexed dbId);
    event Promoted(bytes32 indexed dbId, address rollup);
    event SequencerSet(bytes32 indexed dbId, address sequencer);
    event RolesSet(address owner, address admitter, address anchor, uint64 leaseDuration);

    modifier onlyOwner() {
        require(msg.sender == owner, "Not the fleet's owner");
        _;
    }

    constructor(address owner_, address anchor_, uint64 leaseDuration_) {
        owner = owner_;
        admitter = owner_;
        anchor = anchor_;
        leaseDuration = leaseDuration_ == 0 ? 7 days : leaseDuration_;
        emit RolesSet(owner, admitter, anchor, leaseDuration);
    }

    function domainSeparator() public view returns (bytes32) {
        return EIP712Sig.domain("HPPLiteFleet", "1", address(this));
    }

    function db(bytes32 dbId) external view returns (Db memory) {
        return _dbs[dbId];
    }

    // ============ What is signed ============

    function headDigest(bytes32 dbId, bytes32 instanceId, uint256 fromHeight, uint256 toHeight, bytes32 chainHashAfter,
                        bytes32 stateRoot, bytes32 manifestDigest, uint256 epoch) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(HEAD_TYPEHASH, dbId, instanceId, fromHeight, toHeight, chainHashAfter,
                                                             stateRoot, manifestDigest, epoch)));
    }

    function admitDigest(bytes32 dbId, address dbOwner, address sequencer, uint256 issuedAt) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(ADMIT_TYPEHASH, dbId, dbOwner, sequencer, issuedAt)));
    }

    function retireDigest(bytes32 dbId, uint256 issuedAt) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(RETIRE_TYPEHASH, dbId, issuedAt)));
    }

    function rootDigest(uint256 seq, bytes32 root, bytes32 manifestDigest, uint256 deadline) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(ROOT_TYPEHASH, seq, root, manifestDigest, deadline)));
    }

    function claimDigest(bytes32 dbId, bytes32 instanceId, uint256 nonce, uint256 deadline) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(CLAIM_TYPEHASH, dbId, instanceId, nonce, deadline)));
    }

    /// keccak256(abi.encode(owner, keccak256(name)))
    function dbIdOf(address dbOwner, string calldata name) external pure returns (bytes32) {
        return keccak256(abi.encode(dbOwner, keccak256(bytes(name))));
    }

    /// keccak256(abi.encode(dbId, owner, nameHash, headDigest))
    function leafOf(bytes32 dbId, address dbOwner, bytes32 nameHash, bytes32 hd) public pure returns (bytes32) {
        return keccak256(abi.encode(dbId, dbOwner, nameHash, hd));
    }

    /// Root `seq` is (root, manifestDigest), as RootPosted or the store's index gave them
    function rootIs(uint256 seq, bytes32 root, bytes32 manifestDigest) public view returns (bool) {
        return root != bytes32(0) && rootHash[seq] == keccak256(abi.encode(root, manifestDigest));
    }

    /// A leaf and its path against root `seq`: siblings bottom up, `left[i]` when sibling i is on the left
    function verifyLeaf(uint256 seq, bytes32 root, bytes32 manifestDigest, bytes32 leaf, bytes32[] calldata path, bool[] calldata left)
        external view returns (bool)
    {
        return rootIs(seq, root, manifestDigest) && foldPath(leaf, path, left) == root;
    }

    function foldPath(bytes32 leaf, bytes32[] calldata path, bool[] calldata left) public pure returns (bytes32 h) {
        require(path.length == left.length, "Path and sides differ");
        h = leaf;
        for (uint256 i = 0; i < path.length; i++) h = left[i] ? keccak256(abi.encodePacked(path[i], h)) : keccak256(abi.encodePacked(h, path[i]));
    }

    // ============ Databases ============

    /// The first claim of a dbId: the fleet's owner or admitter admits it (owner, sequencer); the sequencer claims.
    /// `migratedFrom` and the genesis fields name the dedicated rollup a database's history began on (0: none)
    function admitFor(bytes32 dbId, address dbOwner, address sequencer, uint256 issuedAt, bytes calldata admitSig,
                      address migratedFrom, uint256 genesisHeight, bytes32 genesisChain, bytes32 genesisRoot) external {
        address a = admitDigest(dbId, dbOwner, sequencer, issuedAt).recover(admitSig);
        require(a != address(0) && (a == owner || a == admitter), "Not admitted by the fleet");
        require(dbOwner != address(0) && sequencer != address(0), "No owner or sequencer");
        Db storage d = _dbs[dbId];
        require(d.owner == address(0) || d.retired, "Already in the fleet");
        require(issuedAt > d.admittedAt, "Admit not newer");
        uint64 epoch = d.epoch;
        delete _dbs[dbId];
        d.owner = dbOwner;
        d.sequencer = sequencer;
        d.admittedAt = uint64(issuedAt);
        d.epoch = epoch;                              // a re-admitted dbId never reuses an epoch
        d.migratedFrom = migratedFrom;
        d.genesisHeight = genesisHeight;
        d.genesisChain = genesisChain;
        d.genesisRoot = genesisRoot;
        emit Admitted(dbId, dbOwner, sequencer, migratedFrom);
    }

    /// The database's sequencer takes the lease (or renews its own); a new holder moves the epoch
    function claimFor(bytes32 dbId, bytes32 instanceId, uint256 nonce, uint256 deadline, bytes calldata sig) external {
        Db storage d = _live(dbId);
        require(block.timestamp <= deadline, "Expired");
        require(nonce == d.claimNonce, "Bad nonce");
        require(claimDigest(dbId, instanceId, nonce, deadline).recover(sig) == d.sequencer, "Not the database's sequencer");
        d.claimNonce++;
        if (d.instance != instanceId) {
            require(d.instance == bytes32(0) || block.timestamp >= d.until, "Lease held by another instance");
            d.instance = instanceId;
            _moveEpoch(dbId, d);
        }
        d.until = uint64(block.timestamp) + leaseDuration;
        emit Claimed(dbId, instanceId, d.until);
    }

    /// The database's owner ends the lease now (a host lost): the epoch moves, the old holder's names are refused
    function forceExpire(bytes32 dbId) external {
        Db storage d = _live(dbId);
        require(msg.sender == d.owner, "Not the database's owner");
        d.until = uint64(block.timestamp);
        d.instance = bytes32(0);
        _moveEpoch(dbId, d);
    }

    /// The database's owner names another sequencer key; the lease ends with the old key
    function setSequencer(bytes32 dbId, address sequencer) external {
        Db storage d = _live(dbId);
        require(msg.sender == d.owner, "Not the database's owner");
        require(sequencer != address(0), "No sequencer");
        d.sequencer = sequencer;
        d.instance = bytes32(0);
        d.until = uint64(block.timestamp);
        _moveEpoch(dbId, d);
        emit SequencerSet(dbId, sequencer);
    }

    /// The database's owner removes its record (rm): the fleet's list stays true; its names stay at the store
    function retireFor(bytes32 dbId, uint256 issuedAt, bytes calldata sig) external {
        Db storage d = _live(dbId);
        require(issuedAt > d.admittedAt, "Retire not newer");
        require(retireDigest(dbId, issuedAt).recover(sig) == d.owner, "Not the database's owner");
        d.retired = true;
        d.instance = bytes32(0);
        d.until = uint64(block.timestamp);
        _moveEpoch(dbId, d);
        emit Retired(dbId);
    }

    /// The database's owner moves it to a dedicated rollup created with its last head as genesis
    function promote(bytes32 dbId, address rollup) external {
        Db storage d = _live(dbId);
        require(msg.sender == d.owner, "Not the database's owner");
        require(rollup != address(0), "No rollup");
        d.promotedTo = rollup;
        d.retired = true;
        d.instance = bytes32(0);
        d.until = uint64(block.timestamp);
        _moveEpoch(dbId, d);
        emit Promoted(dbId, rollup);
    }

    function _live(bytes32 dbId) internal view returns (Db storage d) {
        d = _dbs[dbId];
        require(d.owner != address(0) && !d.retired, "No such database");
    }

    function _moveEpoch(bytes32 dbId, Db storage d) internal {
        d.epoch++;
        d.epochSince = uint64(rootSeq + 1);           // the next root is the first that may carry heads of this epoch
        emit EpochMoved(dbId, d.epoch, d.epochSince);
    }

    // ============ Roots ============

    /// One root per anchor interval, by the fleet's anchor key; sequences without gaps
    function postRootFor(uint256 seq, bytes32 root, bytes32 manifestDigest, uint256 deadline, bytes calldata sig) external {
        require(block.timestamp <= deadline, "Expired");
        require(seq == rootSeq + 1, "Not the next root");
        require(root != bytes32(0) && manifestDigest != bytes32(0), "Empty root");
        require(rootDigest(seq, root, manifestDigest, deadline).recover(sig) == anchor, "Not the fleet's anchor");
        rootSeq = seq;
        rootHash[seq] = keccak256(abi.encode(root, manifestDigest));
        emit RootPosted(seq, root, manifestDigest);
    }

    // ============ The fleet's roles ============

    function setRoles(address admitter_, address anchor_, uint64 leaseDuration_) external onlyOwner {
        require(anchor_ != address(0) && leaseDuration_ > 0, "No anchor or lease");
        admitter = admitter_;
        anchor = anchor_;
        leaseDuration = leaseDuration_;
        emit RolesSet(owner, admitter, anchor, leaseDuration);
    }

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "No owner");
        owner = newOwner;
        emit RolesSet(owner, admitter, anchor, leaseDuration);
    }
}
