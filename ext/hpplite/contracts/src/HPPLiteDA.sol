// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "./HPPLite.sol";
import "./EIP712Sig.sol";

/**
 * @title HPPLiteDA
 * @notice The rollup's store and its commitments (#33).
 * @dev Version 2. A batch's bytes go into the transaction's log, not into
 *      storage: BatchSubmitted(height, hash, data). Storage keeps what the
 *      chain vouches for: the height, a rolling hash over every batch so
 *      far (chainHash), and the block each batch's log is in (batchBlock,
 *      packed four to a slot), so a reader fetches one block's log by the
 *      height topic and a rebuild recomputes chainHash to the tip.
 *      commitBatch records a batch whose bytes live elsewhere (#11): its
 *      hash, an opaque reference into that store and a pinner's receipt.
 *      The *For forms take the sequencer wallet's EIP-712 signature instead
 *      of its transaction, so a relay may pay the gas (#126); a signature
 *      names one height and can only be used while that height is next.
 *      Version 1 kept the bytes in storage (batchData); readers tell the
 *      two apart by the coordinator's VERSION (7 before, 8 from here on).
 */
contract HPPLiteDA {
    using EIP712Sig for bytes32;

    // ============ Constants ============
    uint256 public constant VERSION = 2;
    bytes32 public constant BATCH_TYPEHASH = keccak256("Batch(bytes32 instanceId,uint256 height,bytes32 batchHash)");

    // ============ State ============
    address public owner;
    HPPLite public coordinator;  // HPPLite contract for lease verification

    uint256 public lastBatchHeight;
    bytes32 public chainHash;    // keccak256(abi.encode(chainHash, height, batchHash)) over every batch, from bytes32(0)
    uint64[] public batchBlock;  // batchBlock[height - 1]: the block whose log carries batch `height`
    uint256 public batchSizeLimit;

    // ============ Events ============
    event BatchSubmitted(uint256 indexed height, bytes32 indexed batchHash, bytes data);
    event BatchCommitted(uint256 indexed height, bytes32 indexed batchHash, bytes ref, bytes receipt);
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
        return EIP712Sig.domain("HPPLiteDA", "2", address(this));
    }

    /// What the sequencer wallet signs for submitBatchFor / commitBatchFor
    function batchDigest(bytes32 instanceId, uint256 height, bytes32 batchHash) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(BATCH_TYPEHASH, instanceId, height, batchHash)));
    }

    function _requireSequencerSig(bytes32 instanceId, uint256 height, bytes32 batchHash, bytes calldata sig) internal view {
        require(address(coordinator) != address(0), "No coordinator set");
        address signer = batchDigest(instanceId, height, batchHash).recover(sig);
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
        _requireSequencerSig(instanceId, height, keccak256(data), sig);
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
        _requireSequencerSig(instanceId, height, batchHash, sig);
        _commit(height, batchHash, ref, receipt);
    }

    function _submit(uint256 height, bytes calldata data) internal {
        require(data.length > 0, "Empty batch");
        require(data.length <= batchSizeLimit, "Batch too large");
        bytes32 batchHash = keccak256(data);
        _record(height, batchHash);
        emit BatchSubmitted(height, batchHash, data);
    }

    function _commit(uint256 height, bytes32 batchHash, bytes calldata ref, bytes calldata receipt) internal {
        require(batchHash != bytes32(0), "Empty hash");
        require(ref.length > 0, "Empty ref");
        _record(height, batchHash);
        emit BatchCommitted(height, batchHash, ref, receipt);
    }

    function _record(uint256 height, bytes32 batchHash) internal {
        require(height == lastBatchHeight + 1, "Non-sequential batch");
        chainHash = keccak256(abi.encode(chainHash, height, batchHash));
        batchBlock.push(_blockNumber());
        lastBatchHeight = height;
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

    /// The block whose log carries batch `height`
    function blockOf(uint256 height) external view returns (uint64) {
        require(height >= 1 && height <= lastBatchHeight, "No such batch");
        return batchBlock[height - 1];
    }

    /// What a reader recomputes over batches 1..lastBatchHeight: the chain hash
    function getDAState() external view returns (uint256 _lastBatchHeight, uint256 _totalBatches, bytes32 _chainHash) {
        return (lastBatchHeight, lastBatchHeight, chainHash);
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
