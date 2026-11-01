// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

/**
 * @title HPPLite
 * @notice HPPLite coordinator - singleton sequencer
 * @dev Manages the sequencer lease and checkpoints, and names the DA contract.
 *      One wallet (the owner, or the address it sets with setSequencer) may
 *      hold the lease; a lease that lapses can only be re-taken by it. The
 *      chain holds the ordered, encrypted history and the state roots; the
 *      key holder rebuilds and compares. No fraud proofs, no challenge period.
 */
/* The one thing the coordinator asks its DA contract (HPPLiteDA imports
** this file, so an interface rather than an import) */
interface IHPPLiteDAHeight {
    function lastBatchHeight() external view returns (uint256);
}

import "./EIP712Sig.sol";

contract HPPLite {
    using EIP712Sig for bytes32;

    // ============ Constants ============
    uint256 public constant LEASE_DURATION = 5 minutes;
    uint256 public constant VERSION = 8;   // 8: signed forms of the lease and checkpoint calls, DA v2 (#33); 7: a checkpoint cannot reach past the DA's height (#44)
    bytes32 public constant LEASE_TYPEHASH = keccak256("Lease(string action,bytes32 instanceId,uint256 nonce)");
    bytes32 public constant CHECKPOINT_TYPEHASH = keccak256("Checkpoint(bytes32 instanceId,uint256 fromHeight,uint256 toHeight,bytes32 stateRoot)");

    // ============ State ============
    address public owner;
    address public sequencer;          // The one wallet allowed to claim the lease (owner by default)

    // Sequencer lease - ensures singleton even with same private key on multiple instances
    address public sequencerWallet;
    bytes32 public sequencerInstance;  // Unique per process (e.g., hash of pid+timestamp+random)
    uint256 public leaseExpiry;
    uint256 public leaseNonce;         // spent by every signed lease call: a signature is used once (#126)

    // Checkpoints
    uint256 public lastCheckpointHeight;
    bytes32 public lastStateRoot;
    uint256 public lastCheckpointTime;
    uint256 public checkpointInterval;

    // Data Availability
    string public daScheme;
    address public daContract;

    // ============ Events ============
    event SequencerClaimed(address indexed wallet, bytes32 indexed instanceId, uint256 leaseExpiry);
    event LeaseReleased(address indexed wallet, bytes32 indexed instanceId);
    event SequencerChanged(address indexed oldSequencer, address indexed newSequencer);
    event LeaseRenewed(address indexed wallet, bytes32 indexed instanceId, uint256 leaseExpiry);
    event CheckpointSubmitted(uint256 indexed fromHeight, uint256 indexed toHeight, bytes32 stateRoot);
    event ConfigUpdated(string indexed key, bytes value);

    // ============ Modifiers ============
    modifier onlyOwner() {
        require(msg.sender == owner, "Not owner");
        _;
    }

    modifier onlyActiveSequencer(bytes32 instanceId) {
        require(msg.sender == sequencer, "Not the sequencer");
        require(msg.sender == sequencerWallet, "Not sequencer wallet");
        require(instanceId == sequencerInstance, "Wrong instance");
        require(block.timestamp < leaseExpiry, "Lease expired");
        _;
    }

    // ============ Constructor ============
    constructor(
        uint256 _checkpointInterval,
        string memory _daScheme,
        address _daContract
    ) {
        owner = msg.sender;
        sequencer = msg.sender;
        checkpointInterval = _checkpointInterval > 0 ? _checkpointInterval : 100;
        daScheme = bytes(_daScheme).length > 0 ? _daScheme : "hppda";
        daContract = _daContract;
        lastCheckpointTime = block.timestamp;
    }

    // ============ Sequencer Lease Functions ============

    /**
     * @notice Claim sequencer role (when lease expired or first time)
     * @param instanceId Unique identifier for this process instance
     */
    function claimSequencer(bytes32 instanceId) external {
        _claim(msg.sender, instanceId);
    }

    function _claim(address wallet, bytes32 instanceId) internal {
        // Permissionless claiming was a leftover of the witness design: any
        // wallet could take an expired lease and post a batch nobody else can
        // decrypt, stopping every rebuild at that height for good.
        require(wallet == sequencer, "Not the sequencer");
        require(
            sequencerWallet == address(0) || block.timestamp >= leaseExpiry,
            "Lease still active"
        );
        require(instanceId != bytes32(0), "Invalid instance ID");

        sequencerWallet = wallet;
        sequencerInstance = instanceId;
        leaseExpiry = block.timestamp + LEASE_DURATION;

        emit SequencerClaimed(wallet, instanceId, leaseExpiry);
    }

    /**
     * @notice Renew lease (must be current sequencer with matching instance)
     * @param instanceId Must match current sequencer instance
     */
    function renewLease(bytes32 instanceId) external onlyActiveSequencer(instanceId) {
        _renew(msg.sender, instanceId);
    }

    function _renew(address wallet, bytes32 instanceId) internal {
        leaseExpiry = block.timestamp + LEASE_DURATION;
        emit LeaseRenewed(wallet, instanceId, leaseExpiry);
    }

    // ============ Signed forms (#33, #126) ============
    // The sequencer wallet signs, anyone submits and pays the gas. A lease
    // signature carries the contract's nonce and is spent by its use; a
    // checkpoint signature names its range, which must be the next one.

    function domainSeparator() public view returns (bytes32) {
        return EIP712Sig.domain("HPPLite", "8", address(this));
    }

    /// What the sequencer wallet signs for claimSequencerFor ("claim"), renewLeaseFor ("renew"), releaseLeaseFor ("release")
    function leaseDigest(string memory action, bytes32 instanceId, uint256 nonce) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(LEASE_TYPEHASH, keccak256(bytes(action)), instanceId, nonce)));
    }

    /// What the sequencer wallet signs for submitCheckpointFor
    function checkpointDigest(bytes32 instanceId, uint256 fromHeight, uint256 toHeight, bytes32 stateRoot) public view returns (bytes32) {
        return domainSeparator().digest(keccak256(abi.encode(CHECKPOINT_TYPEHASH, instanceId, fromHeight, toHeight, stateRoot)));
    }

    function _spendLeaseSig(string memory action, bytes32 instanceId, uint256 nonce, bytes calldata sig) internal returns (address wallet) {
        require(nonce == leaseNonce, "Wrong nonce");
        wallet = leaseDigest(action, instanceId, nonce).recover(sig);
        require(wallet != address(0), "Bad signature");
        leaseNonce = nonce + 1;
    }

    function claimSequencerFor(bytes32 instanceId, uint256 nonce, bytes calldata sig) external {
        _claim(_spendLeaseSig("claim", instanceId, nonce, sig), instanceId);
    }

    function renewLeaseFor(bytes32 instanceId, uint256 nonce, bytes calldata sig) external {
        address wallet = _spendLeaseSig("renew", instanceId, nonce, sig);
        _requireActive(wallet, instanceId);
        _renew(wallet, instanceId);
    }

    function releaseLeaseFor(bytes32 instanceId, uint256 nonce, bytes calldata sig) external {
        address wallet = _spendLeaseSig("release", instanceId, nonce, sig);
        _release(wallet, instanceId);
    }

    function submitCheckpointFor(bytes32 instanceId, uint256 fromHeight, uint256 toHeight, bytes32 stateRoot, bytes calldata sig) external {
        address wallet = checkpointDigest(instanceId, fromHeight, toHeight, stateRoot).recover(sig);
        require(wallet != address(0), "Bad signature");
        _requireActive(wallet, instanceId);
        _checkpoint(fromHeight, toHeight, stateRoot);
    }

    /// onlyActiveSequencer for a recovered signer instead of msg.sender
    function _requireActive(address wallet, bytes32 instanceId) internal view {
        require(wallet == sequencer, "Not the sequencer");
        require(wallet == sequencerWallet, "Not sequencer wallet");
        require(instanceId == sequencerInstance, "Wrong instance");
        require(block.timestamp < leaseExpiry, "Lease expired");
    }

    /**
     * @notice Check if current sequencer lease is active
     */
    function isLeaseActive() external view returns (bool) {
        return sequencerWallet != address(0) && block.timestamp < leaseExpiry;
    }

    /**
     * @notice Get time remaining on lease (0 if expired)
     */
    function leaseTimeRemaining() external view returns (uint256) {
        if (block.timestamp >= leaseExpiry) return 0;
        return leaseExpiry - block.timestamp;
    }

    // ============ Checkpoint Functions ============

    /**
     * @notice Submit checkpoint (simplified - no attestations in singleton model)
     * @param instanceId Must match current sequencer instance
     * @param fromHeight Start of checkpoint range
     * @param toHeight End of checkpoint range
     * @param stateRoot Post-state root after applying batches
     */
    function submitCheckpoint(
        bytes32 instanceId,
        uint256 fromHeight,
        uint256 toHeight,
        bytes32 stateRoot
    ) external onlyActiveSequencer(instanceId) {
        _checkpoint(fromHeight, toHeight, stateRoot);
    }

    function _checkpoint(uint256 fromHeight, uint256 toHeight, bytes32 stateRoot) internal {
        require(fromHeight == lastCheckpointHeight + 1 || lastCheckpointHeight == 0, "Non-sequential");
        require(toHeight >= fromHeight, "Invalid range");
        // a root for batches the DA does not hold would be a root nobody can
        // check (#44); the DA contract is this rollup's, set by the factory
        require(daContract == address(0) || toHeight <= IHPPLiteDAHeight(daContract).lastBatchHeight(), "Beyond DA height");

        lastCheckpointHeight = toHeight;
        lastStateRoot = stateRoot;
        lastCheckpointTime = block.timestamp;

        // Auto-renew lease on activity
        leaseExpiry = block.timestamp + LEASE_DURATION;

        emit CheckpointSubmitted(fromHeight, toHeight, stateRoot);
    }

    // ============ Admin Functions ============

    function transferOwnership(address newOwner) external onlyOwner {
        require(newOwner != address(0), "Invalid owner");
        // The factory creates the rollup and hands it to the wallet: the
        // sequencer follows the owner unless it was set apart explicitly
        if (sequencer == owner) {
            emit SequencerChanged(sequencer, newOwner);
            sequencer = newOwner;
            leaseExpiry = block.timestamp;   // the old key's lease ends with its role
        }
        owner = newOwner;
    }

    /**
     * @notice Name the wallet allowed to claim the lease (hot key), distinct from the owner
     */
    function setSequencer(address newSequencer) external onlyOwner {
        require(newSequencer != address(0), "Invalid sequencer");
        emit SequencerChanged(sequencer, newSequencer);
        sequencer = newSequencer;
        leaseExpiry = block.timestamp;       // a rotated-out key cannot keep posting on its old lease
    }

    /**
     * @notice Give the lease up now (graceful close) - the active instance itself, no owner needed
     */
    function releaseLease(bytes32 instanceId) external {
        _release(msg.sender, instanceId);
    }

    function _release(address wallet, bytes32 instanceId) internal {
        require(wallet == sequencerWallet, "Not sequencer wallet");
        require(instanceId == sequencerInstance, "Wrong instance");
        require(block.timestamp < leaseExpiry, "Lease expired");
        leaseExpiry = block.timestamp;
        emit LeaseReleased(wallet, instanceId);
    }

    function setDAConfig(string calldata _scheme, address _da) external onlyOwner {
        daScheme = _scheme;
        daContract = _da;
        emit ConfigUpdated("daConfig", abi.encode(_scheme, _da));
    }

    function setCheckpointInterval(uint256 _interval) external onlyOwner {
        checkpointInterval = _interval;
    }

    /**
     * @notice Emergency: force expire lease (owner only)
     * @dev Use if sequencer is misbehaving and needs to be replaced
     */
    function forceExpireLease() external onlyOwner {
        leaseExpiry = block.timestamp;
    }

    // ============ View Functions ============

    function getState() external view returns (
        address _owner,
        address _sequencerWallet,
        bytes32 _sequencerInstance,
        uint256 _leaseExpiry,
        uint256 _lastCheckpointHeight,
        bytes32 _lastStateRoot,
        uint256 _lastCheckpointTime
    ) {
        return (
            owner,
            sequencerWallet,
            sequencerInstance,
            leaseExpiry,
            lastCheckpointHeight,
            lastStateRoot,
            lastCheckpointTime
        );
    }

    function getSystemConfig() external view returns (
        string memory _daScheme,
        address _daContract,
        uint256 _version,
        uint256 _chainId,
        uint256 _checkpointInterval,
        uint256 _leaseDuration
    ) {
        return (daScheme, daContract, VERSION, block.chainid, checkpointInterval, LEASE_DURATION);
    }

    function version() external pure returns (uint256) {
        return VERSION;
    }
}
