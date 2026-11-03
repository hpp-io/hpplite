// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./HPPLite.sol";
import "./EIP712Sig.sol";

/**
 * @title HPPLiteDA
 * @notice The rollup's store and its commitments (#33, #164).
 * @dev Version 3. A batch's bytes go into the transaction's log, not into
 *      storage: BatchSubmitted(height, hash, data, prevBlock). commitBatch
 *      records a batch whose bytes live elsewhere (#11): its hash, an opaque
 *      reference into that store and a pinner's receipt. commitRange (#164)
 *      anchors a whole range of such batches in one transaction: the chain
 *      hash the sequencer folded over them (the same fold, batch by batch,
 *      that _record does; a reader folds the manifest's hashes and refuses a
 *      manifest that does not reach it), the state root after the last, the
 *      digest of a manifest at the store naming every batch, and the store's
 *      receipt for the manifest. The anchor is the coordinator's checkpoint.
 *      Storage keeps only what every commitment rewrites, two slots: the
 *      height and the block of the last commitment, packed, and the chain
 *      hash. Nothing grows with the history: every event names the block of
 *      the commitment before it, so a reader walks them backward from
 *      lastBlock() (version 2 kept a block per height, batchBlock, a storage
 *      slot per batch, and blockOf(height)).
 *      The *For forms take the sequencer wallet's EIP-712 signature instead
 *      of its transaction, so a relay may pay the gas (#126); a signature
 *      names one height or range and can only be used while that is next.
 *      Readers tell the versions apart by the coordinator's VERSION (7:
 *      bytes in storage, 8: version 2, 9: this).
 */
contract HPPLiteDA {
    using EIP712Sig for bytes32;

    // ============ Constants ============
    uint256 public constant VERSION = 3;
    bytes32 public constant BATCH_TYPEHASH = keccak256("Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)");
    bytes32 public constant RANGE_TYPEHASH = keccak256("Range(bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 chainHashAfter,bytes32 stateRoot,bytes32 manifestDigest)");

    // ============ State ============
    address public owner;
    HPPLite public coordinator;  // HPPLite contract for lease verification

    uint64 private _lastBatchHeight;
    uint64 private _lastBlock;    // the block whose log carries the last commitment
    bytes32 public chainHash;    // keccak256(abi.encode(chainHash, height, batchHash)) over every batch, from bytes32(0)
    uint256 public batchSizeLimit;

    // ============ Events ============
    event BatchSubmitted(uint256 indexed height, bytes32 indexed batchHash, bytes data, uint256 prevBlock);
    event BatchCommitted(uint256 indexed height, bytes32 indexed batchHash, bytes ref, bytes receipt, uint256 prevBlock);
    event RangeCommitted(uint256 indexed fromHeight, uint256 indexed toHeight, bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest, bytes receipt, uint256 prevBlock);
    event CoordinatorChanged(address indexed oldCoordinator, address indexed newCoordinator);

    // ============ Modifiers ============
    modifier onlyOwner() {
        require(msg.sender == owner, "Not owner");
        _;
    }

    modifier onlyActiveSequencer(bytes32 instanceId) {
        require(address(coordinator) != address(0), "No coordinator set");
        require(msg.sender == coordinator.sequencerWallet(), "Not sequencer wallet");
        _requireActiveInstance(instanceId);
        _;
    }

    function _requireActiveInstance(bytes32 instanceId) internal view {
        require(instanceId == coordinator.sequencerInstance(), "Wrong instance");
        require(block.timestamp < coordinator.leaseExpiry(), "Lease expired");
    }

    // ============ Constructor ============
    constructor(address _coordinator, uint256 _batchSizeLimit) {
        owner = msg.sender;
        coordinator = HPPLite(_coordinator);
        batchSizeLimit = _batchSizeLimit > 0 ? _batchSizeLimit : 128 * 1024;
    }

    // ============ Signed forms ============

    function domainSeparator() public view returns (bytes32) {
        return EIP712Sig.domain("HPPLiteDA", "3", address(this));
    }

    /// What the sequencer wallet signs for submitBatchFor / commitBatchFor
    function batchDigest(bytes32 instanceId, uint256 height, bytes32 batchHash) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(BATCH_TYPEHASH, instanceId, height, batchHash)));
    }

    /// What the sequencer wallet signs for commitRangeFor
    function rangeDigest(bytes32 instanceId, uint256 fromHeight, uint256 toHeight, bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest)
        public view returns (bytes32)
    {
        return domainSeparator().digest(keccak256(abi.encode(RANGE_TYPEHASH, instanceId, fromHeight, toHeight, chainHashAfter, stateRoot, manifestDigest)));
    }

    function _requireSequencerSig(bytes32 instanceId, bytes32 digest, bytes calldata sig) internal view {
        require(address(coordinator) != address(0), "No coordinator set");
        address signer = digest.recover(sig);
        require(signer != address(0) && signer == coordinator.sequencerWallet(), "Not sequencer wallet");
        _requireActiveInstance(instanceId);
    }

    // ============ Batch Functions ============

    /**
     * @notice Post a batch: its bytes in this transaction's log, its hash in the chain hash
     * @param instanceId Sequencer instance ID (must match lease)
     * @param height Batch height (must be sequential)
     * @param data Batch data
     */
    function submitBatch(bytes32 instanceId, uint256 height, bytes calldata data) external onlyActiveSequencer(instanceId) {
        _submit(height, data);
    }

    /// submitBatch by anyone, with the sequencer wallet's signature over (instanceId, height, keccak256(data))
    function submitBatchFor(bytes32 instanceId, uint256 height, bytes calldata data, bytes calldata sig) external {
        _requireSequencerSig(instanceId, batchDigest(instanceId, height, keccak256(data)), sig);
        _submit(height, data);
    }

    /**
     * @notice Commit a batch whose bytes live in another store (#11)
     * @param batchHash keccak256 of the batch bytes
     * @param ref Where the bytes are: opaque to the chain (a CID, an object key)
     * @param receipt A pinner's signed promise to hold them, or empty
     */
    function commitBatch(bytes32 instanceId, uint256 height, bytes32 batchHash, bytes calldata ref, bytes calldata receipt)
        external onlyActiveSequencer(instanceId)
    {
        _commit(height, batchHash, ref, receipt);
    }

    function commitBatchFor(bytes32 instanceId, uint256 height, bytes32 batchHash, bytes calldata ref, bytes calldata receipt, bytes calldata sig)
        external
    {
        _requireSequencerSig(instanceId, batchDigest(instanceId, height, batchHash), sig);
        _commit(height, batchHash, ref, receipt);
    }

    /**
     * @notice Anchor a range of batches held by a store (#164), one transaction for all of them
     * @param fromHeight The first height of the range: the one after the last commitment
     * @param toHeight The last height of the range
     * @param chainHashAfter The chain hash after the range: the fold of _record over every batch in it, done by the sequencer
     * @param stateRoot The state root after toHeight: this anchor is the checkpoint
     * @param manifestDigest The digest of the manifest at the store: per height its batch hash, reference and receipt
     * @param receipt The store's signed promise to hold the manifest, or empty
     */
    function commitRange(bytes32 instanceId, uint256 fromHeight, uint256 toHeight, bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest, bytes calldata receipt)
        external
    {
        _range(msg.sender, instanceId, fromHeight, toHeight, chainHashAfter, stateRoot, manifestDigest, receipt);
    }

    function commitRangeFor(bytes32 instanceId, uint256 fromHeight, uint256 toHeight, bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest, bytes calldata receipt, bytes calldata sig)
        external
    {
        address signer = rangeDigest(instanceId, fromHeight, toHeight, chainHashAfter, stateRoot, manifestDigest).recover(sig);
        require(signer != address(0), "Bad signature");
        _range(signer, instanceId, fromHeight, toHeight, chainHashAfter, stateRoot, manifestDigest, receipt);
    }

    function _submit(uint256 height, bytes calldata data) internal {
        require(data.length > 0, "Empty batch");
        require(data.length <= batchSizeLimit, "Batch too large");
        bytes32 batchHash = keccak256(data);
        uint256 prevBlock = _record(height, batchHash);
        emit BatchSubmitted(height, batchHash, data, prevBlock);
    }

    function _commit(uint256 height, bytes32 batchHash, bytes calldata ref, bytes calldata receipt) internal {
        require(batchHash != bytes32(0), "Empty hash");
        require(ref.length > 0, "Empty ref");
        uint256 prevBlock = _record(height, batchHash);
        emit BatchCommitted(height, batchHash, ref, receipt, prevBlock);
    }

    /// The lease and the instance are the coordinator's to check, in recordAnchor: one call per anchor
    function _range(address wallet, bytes32 instanceId, uint256 fromHeight, uint256 toHeight, bytes32 chainHashAfter, bytes32 stateRoot, bytes32 manifestDigest, bytes calldata receipt) internal {
        require(address(coordinator) != address(0), "No coordinator set");
        require(fromHeight == uint256(_lastBatchHeight) + 1, "Non-sequential batch");
        require(toHeight >= fromHeight, "Invalid range");
        require(chainHashAfter != bytes32(0), "Empty hash");
        require(manifestDigest != bytes32(0), "Empty manifest");
        coordinator.recordAnchor(wallet, instanceId, toHeight, stateRoot);
        uint256 prevBlock = _lastBlock;
        _lastBatchHeight = uint64(toHeight);
        _lastBlock = _blockNumber();
        chainHash = chainHashAfter;
        emit RangeCommitted(fromHeight, toHeight, chainHashAfter, stateRoot, manifestDigest, receipt, prevBlock);
    }

    function _record(uint256 height, bytes32 batchHash) internal returns (uint256 prevBlock) {
        require(height == uint256(_lastBatchHeight) + 1, "Non-sequential batch");
        chainHash = keccak256(abi.encode(chainHash, height, batchHash));
        prevBlock = _lastBlock;
        _lastBatchHeight = uint64(height);
        _lastBlock = _blockNumber();
    }

    /**
     * @dev The block a reader asks eth_getLogs for. On an Arbitrum-style
     *      chain (HPP is one) block.number is the parent chain's block, and
     *      the chain's own is ArbSys(0x64).arbBlockNumber(); where there is
     *      no ArbSys (anvil, Ethereum itself) the staticcall returns nothing
     *      and block.number is right. Found on HPP Sepolia: the first v2
     *      factory recorded L1 blocks and no replica could fetch a log.
     */
    function _blockNumber() internal view returns (uint64) {
        (bool ok, bytes memory r) = address(100).staticcall(abi.encodeWithSignature("arbBlockNumber()"));
        if (ok && r.length == 32) return uint64(abi.decode(r, (uint256)));
        return uint64(block.number);
    }

    // ============ View Functions ============

    function lastBatchHeight() public view returns (uint256) {
        return _lastBatchHeight;
    }

    /// The block whose log carries the last commitment; its event names the block of the one before, and so on back to the first
    function lastBlock() external view returns (uint256) {
        return _lastBlock;
    }

    /// What a reader recomputes over batches 1..lastBatchHeight: the chain hash
    function getDAState() external view returns (uint256 lastBatchHeight_, uint256 _totalBatches, bytes32 _chainHash) {
        return (_lastBatchHeight, _lastBatchHeight, chainHash);
    }

    // ============ Admin Functions ============

    function setCoordinator(address _coordinator) external onlyOwner {
        address old = address(coordinator);
        coordinator = HPPLite(_coordinator);
        emit CoordinatorChanged(old, _coordinator);
    }

    function setBatchSizeLimit(uint256 _limit) external onlyOwner {
        batchSizeLimit = _limit;
    }

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "Invalid owner");
        owner = newOwner;
    }
}
