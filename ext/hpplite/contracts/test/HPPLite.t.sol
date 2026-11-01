// SPDX-License-Identifier: MIT
pragma solidity ^0.8.20;

import "forge-std/Test.sol";
import "../src/HPPLite.sol";
import "../src/HPPLiteDA.sol";
import "../src/HPPLiteFactory.sol";

contract HPPLiteTest is Test {
    HPPLite public hpplite;
    HPPLiteDA public da;

    address owner = address(this);
    address sequencer;
    uint256 sequencerKey;

    bytes32 instanceId1 = keccak256("instance1");
    bytes32 instanceId2 = keccak256("instance2");

    function setUp() public {
        sequencerKey = 0x1;
        sequencer = vm.addr(sequencerKey);

        // Deploy HPPLite first
        hpplite = new HPPLite(
            100,            // checkpointInterval
            "hppda",        // daScheme
            address(0)      // daContract (set later)
        );
        // the owner (this test) delegates the lease to a separate hot wallet
        hpplite.setSequencer(sequencer);

        // Deploy DA contract with HPPLite as coordinator
        da = new HPPLiteDA(address(hpplite), 128 * 1024);

        // Link DA to HPPLite
        hpplite.setDAConfig("hppda", address(da));
    }

    function test_InitialState() public view {
        assertEq(hpplite.owner(), owner);
        assertEq(hpplite.sequencerWallet(), address(0));
        assertEq(hpplite.sequencerInstance(), bytes32(0));
        assertEq(hpplite.checkpointInterval(), 100);
        assertEq(hpplite.daContract(), address(da));
        assertEq(hpplite.VERSION(), 8);
        assertEq(da.VERSION(), 2);
    }

    function test_ClaimSequencer() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        assertEq(hpplite.sequencerWallet(), sequencer);
        assertEq(hpplite.sequencerInstance(), instanceId1);
        assertTrue(hpplite.isLeaseActive());
    }

    function test_ClaimSequencer_LeaseActive() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        // Another instance of the sequencer wallet tries to claim while lease active
        vm.prank(sequencer);
        vm.expectRevert("Lease still active");
        hpplite.claimSequencer(instanceId2);
    }

    function test_ClaimSequencer_AfterLeaseExpires() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        // Fast forward past lease duration
        vm.warp(block.timestamp + 6 minutes);
        assertFalse(hpplite.isLeaseActive());

        // Only the sequencer wallet can re-take it (a new instance of the same wallet)
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId2);

        assertEq(hpplite.sequencerWallet(), sequencer);
        assertEq(hpplite.sequencerInstance(), instanceId2);
    }

    function test_ClaimSequencer_StrangerRejected() public {
        address stranger = address(0x999);
        // before any claim
        vm.prank(stranger);
        vm.expectRevert("Not the sequencer");
        hpplite.claimSequencer(instanceId2);
        // while active
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);
        vm.prank(stranger);
        vm.expectRevert("Not the sequencer");
        hpplite.claimSequencer(instanceId2);
        // and after expiry - the hole external review found
        vm.warp(block.timestamp + 6 minutes);
        vm.prank(stranger);
        vm.expectRevert("Not the sequencer");
        hpplite.claimSequencer(instanceId2);
        assertEq(hpplite.sequencerWallet(), sequencer);
    }

    function test_SetSequencer_AndOwnerFollows() public {
        HPPLite fresh = new HPPLite(100, "hppda", address(0));
        assertEq(fresh.sequencer(), address(this));          // owner by default
        address newOwner = address(0xABCD);
        fresh.transferOwnership(newOwner);
        assertEq(fresh.sequencer(), newOwner);               // follows the owner when not set apart
        vm.prank(newOwner);
        fresh.setSequencer(sequencer);
        vm.prank(newOwner);
        fresh.transferOwnership(address(0xBEEF));
        assertEq(fresh.sequencer(), sequencer);              // explicit sequencer stays
        vm.prank(address(0x999));
        vm.expectRevert("Not owner");
        fresh.setSequencer(address(0x999));
    }

    function test_SetSequencer_RevokesLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);
        assertTrue(hpplite.isLeaseActive());
        address newKey = address(0x7777);
        hpplite.setSequencer(newKey);
        // the old key's lease is gone: it cannot renew, checkpoint or post
        assertFalse(hpplite.isLeaseActive());
        vm.prank(sequencer);
        vm.expectRevert("Not the sequencer");
        hpplite.renewLease(instanceId1);
        vm.prank(sequencer);
        vm.expectRevert("Lease expired");
        da.submitBatch(instanceId1, 1, "x");
        // and the new key can claim at once
        vm.prank(newKey);
        hpplite.claimSequencer(instanceId2);
        assertEq(hpplite.sequencerWallet(), newKey);
    }

    function test_ReleaseLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);
        assertTrue(hpplite.isLeaseActive());
        // wrong instance cannot release
        vm.prank(sequencer);
        vm.expectRevert("Wrong instance");
        hpplite.releaseLease(instanceId2);
        // the holder can, without the owner
        vm.prank(sequencer);
        hpplite.releaseLease(instanceId1);
        assertFalse(hpplite.isLeaseActive());
        // and claim again at once
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId2);
        assertTrue(hpplite.isLeaseActive());
    }

    function test_RenewLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        uint256 originalExpiry = hpplite.leaseExpiry();

        // Fast forward 2 minutes
        vm.warp(block.timestamp + 2 minutes);

        vm.prank(sequencer);
        hpplite.renewLease(instanceId1);

        assertGt(hpplite.leaseExpiry(), originalExpiry);
    }

    function test_RenewLease_WrongInstance() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        vm.prank(sequencer);
        vm.expectRevert("Wrong instance");
        hpplite.renewLease(instanceId2);
    }

    /* the DA holds n batches, posted by the sequencer under instanceId1 */
    function postBatches(uint256 n) internal {
        for (uint256 h = da.lastBatchHeight() + 1; h <= n; h++) {
            vm.prank(sequencer);
            da.submitBatch(instanceId1, h, abi.encodePacked("batch ", h));
        }
    }

    function test_SubmitCheckpoint_BeyondDAHeight() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);
        postBatches(3);
        // a checkpoint cannot claim a root for batches the DA does not hold (#44)
        vm.prank(sequencer);
        vm.expectRevert("Beyond DA height");
        hpplite.submitCheckpoint(instanceId1, 1, 4, keccak256("state"));
        vm.prank(sequencer);
        hpplite.submitCheckpoint(instanceId1, 1, 3, keccak256("state"));
        assertEq(hpplite.lastCheckpointHeight(), 3);
    }

    function test_SubmitCheckpoint() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);
        postBatches(10);

        bytes32 stateRoot = keccak256("state");

        vm.prank(sequencer);
        hpplite.submitCheckpoint(instanceId1, 1, 10, stateRoot);

        assertEq(hpplite.lastCheckpointHeight(), 10);
        assertEq(hpplite.lastStateRoot(), stateRoot);
    }

    function test_SubmitCheckpoint_AutoRenewsLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);
        postBatches(10);

        // Fast forward 4 minutes (almost expired)
        vm.warp(block.timestamp + 4 minutes);

        vm.prank(sequencer);
        hpplite.submitCheckpoint(instanceId1, 1, 10, keccak256("state"));

        // Lease should be renewed
        assertTrue(hpplite.isLeaseActive());
        assertGt(hpplite.leaseTimeRemaining(), 4 minutes);
    }

    function test_SubmitCheckpoint_LeaseExpired() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        // Fast forward past lease
        vm.warp(block.timestamp + 6 minutes);

        vm.prank(sequencer);
        vm.expectRevert("Lease expired");
        hpplite.submitCheckpoint(instanceId1, 1, 10, keccak256("state"));
    }

    function test_SubmitCheckpoint_WrongInstance() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        vm.prank(sequencer);
        vm.expectRevert("Wrong instance");
        hpplite.submitCheckpoint(instanceId2, 1, 10, keccak256("state"));
    }

    function test_ForceExpireLease() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        assertTrue(hpplite.isLeaseActive());

        // Owner can force expire
        hpplite.forceExpireLease();

        assertFalse(hpplite.isLeaseActive());
    }

    function test_GetState() public {
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId1);

        (
            address _owner,
            address _sequencerWallet,
            bytes32 _sequencerInstance,
            uint256 _leaseExpiry,
            uint256 _lastCheckpointHeight,
            bytes32 _lastStateRoot,
            uint256 _lastCheckpointTime
        ) = hpplite.getState();

        assertEq(_owner, owner);
        assertEq(_sequencerWallet, sequencer);
        assertEq(_sequencerInstance, instanceId1);
        assertGt(_leaseExpiry, block.timestamp);
        assertEq(_lastCheckpointHeight, 0);
    }

    function test_GetSystemConfig() public view {
        (
            string memory daScheme,
            address daContract,
            uint256 version,
            uint256 chainId,
            uint256 checkpointInterval,
            uint256 leaseDuration
        ) = hpplite.getSystemConfig();

        assertEq(daScheme, "hppda");
        assertEq(daContract, address(da));
        assertEq(version, 8);
        assertEq(checkpointInterval, 100);
        assertEq(leaseDuration, 5 minutes);
    }

    function test_TransferOwnership() public {
        address newOwner = address(0x123);
        hpplite.transferOwnership(newOwner);
        assertEq(hpplite.owner(), newOwner);
    }
}

contract HPPLiteDATest is Test {
    HPPLite public hpplite;
    HPPLiteDA public da;

    address sequencer;
    uint256 sequencerKey = 0x1;                 // the signed forms need a key, not just an address
    bytes32 instanceId = keccak256("instance1");
    bytes32 instanceId2 = keccak256("instance2");

    function setUp() public {
        sequencer = vm.addr(sequencerKey);

        hpplite = new HPPLite(100, "hppda", address(0));
        da = new HPPLiteDA(address(hpplite), 128 * 1024);
        hpplite.setDAConfig("hppda", address(da));
        hpplite.setSequencer(sequencer);

        // Claim sequencer
        vm.prank(sequencer);
        hpplite.claimSequencer(instanceId);
    }

    event BatchSubmitted(uint256 indexed height, bytes32 indexed batchHash, bytes data);
    event BatchCommitted(uint256 indexed height, bytes32 indexed batchHash, bytes ref, bytes receipt);

    function chain(bytes32 prev, uint256 height, bytes memory data) internal pure returns (bytes32) {
        return keccak256(abi.encode(prev, height, keccak256(data)));
    }

    function sign(uint256 key, bytes32 digest) internal pure returns (bytes memory) {
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(key, digest);
        return abi.encodePacked(r, s, v);
    }

    /* #33: the bytes go into the log, storage keeps the height, the rolling hash and the block */
    function test_SubmitBatch() public {
        bytes memory batchData = '{"ops":[{"type":"insert"}]}';

        vm.roll(1234);
        vm.expectEmit(true, true, false, true, address(da));
        emit BatchSubmitted(1, keccak256(batchData), batchData);
        vm.prank(sequencer);
        da.submitBatch(instanceId, 1, batchData);

        assertEq(da.lastBatchHeight(), 1);
        assertEq(da.chainHash(), chain(bytes32(0), 1, batchData));
        assertEq(da.blockOf(1), 1234);
        vm.expectRevert("No such batch");
        da.blockOf(2);
    }

    /* On an Arbitrum-style chain block.number is the parent chain's block; the
    ** block a reader can fetch a log from is ArbSys(0x64).arbBlockNumber() */
    function test_BatchBlock_IsTheChainsOwnBlockUnderArbSys() public {
        // a stand-in ArbSys: any call returns 7654321
        bytes memory code = abi.encodePacked(hex"7f", bytes32(uint256(7654321)), hex"60005260206000f3");
        vm.etch(address(100), code);
        vm.roll(99);                                      // what block.number says: the parent chain's
        vm.prank(sequencer);
        da.submitBatch(instanceId, 1, "on an L2");
        assertEq(da.blockOf(1), 7654321);
    }

    /* The cost cut this issue is for: a 1 KB batch below 100 k gas (storage took ~700 k) */
    function test_SubmitBatch_Gas() public {
        bytes memory batchData = new bytes(1024);
        for (uint256 i = 0; i < batchData.length; i++) batchData[i] = bytes1(uint8(32 + i % 90));
        vm.prank(sequencer);
        da.submitBatch(instanceId, 1, "warm up");        // the storage slots exist after the first batch
        vm.prank(sequencer);
        uint256 gas = gasleft();
        da.submitBatch(instanceId, 2, batchData);
        gas -= gasleft();
        emit log_named_uint("gas for a 1 KB batch", gas);
        assertLt(gas, 100000);
    }

    function test_ChainHash_TenBatches() public {
        bytes32 h = bytes32(0);
        vm.startPrank(sequencer);
        for (uint256 i = 1; i <= 10; i++) {
            bytes memory data = abi.encodePacked("batch ", i);
            vm.roll(100 + i);
            da.submitBatch(instanceId, i, data);
            h = chain(h, i, data);
            assertEq(da.chainHash(), h);
            assertEq(da.blockOf(i), 100 + i);
        }
        vm.stopPrank();
        (uint256 last, uint256 total, bytes32 ch) = da.getDAState();
        assertEq(last, 10);
        assertEq(total, 10);
        assertEq(ch, h);
    }

    function test_SubmitBatchFor_RelayPaysWithSequencerSignature() public {
        bytes memory batchData = "signed batch";
        bytes memory sig = sign(sequencerKey, da.batchDigest(instanceId, 1, keccak256(batchData)));
        address relay = address(0xBEEF);
        vm.prank(relay);
        da.submitBatchFor(instanceId, 1, batchData, sig);
        assertEq(da.lastBatchHeight(), 1);
        assertEq(da.chainHash(), chain(bytes32(0), 1, batchData));

        // the same signature again: that height is taken
        vm.prank(relay);
        vm.expectRevert("Non-sequential batch");
        da.submitBatchFor(instanceId, 1, batchData, sig);
    }

    function test_SubmitBatchFor_WrongSignerOrData() public {
        bytes memory batchData = "signed batch";
        bytes memory other = sign(0x2, da.batchDigest(instanceId, 1, keccak256(batchData)));
        vm.expectRevert("Not sequencer wallet");
        da.submitBatchFor(instanceId, 1, batchData, other);

        bytes memory sig = sign(sequencerKey, da.batchDigest(instanceId, 1, keccak256(batchData)));
        vm.expectRevert("Not sequencer wallet");          // other bytes: another hash, so another signer recovers
        da.submitBatchFor(instanceId, 1, "tampered", sig);

        bytes memory wrongInstance = sign(sequencerKey, da.batchDigest(keccak256("other"), 1, keccak256(batchData)));
        vm.expectRevert("Wrong instance");
        da.submitBatchFor(keccak256("other"), 1, batchData, wrongInstance);

        vm.expectRevert("Not sequencer wallet");
        da.submitBatchFor(instanceId, 1, batchData, "short");
    }

    function test_SubmitBatchFor_LeaseExpired() public {
        bytes memory sig = sign(sequencerKey, da.batchDigest(instanceId, 1, keccak256("x")));
        vm.warp(block.timestamp + 6 minutes);
        vm.expectRevert("Lease expired");
        da.submitBatchFor(instanceId, 1, "x", sig);
    }

    function test_CommitBatch_OffChainStore() public {
        bytes32 h = keccak256("bytes held elsewhere");
        vm.expectEmit(true, true, false, true, address(da));
        emit BatchCommitted(1, h, "bafkreiabc", "receipt");
        vm.prank(sequencer);
        da.commitBatch(instanceId, 1, h, "bafkreiabc", "receipt");
        assertEq(da.lastBatchHeight(), 1);
        assertEq(da.chainHash(), keccak256(abi.encode(bytes32(0), uint256(1), h)));

        vm.prank(sequencer);
        vm.expectRevert("Empty ref");
        da.commitBatch(instanceId, 2, h, "", "");
        vm.prank(sequencer);
        vm.expectRevert("Empty hash");
        da.commitBatch(instanceId, 2, bytes32(0), "ref", "");

        // the signed form, by a relay
        bytes memory sig = sign(sequencerKey, da.batchDigest(instanceId, 2, h));
        vm.prank(address(0xBEEF));
        da.commitBatchFor(instanceId, 2, h, "bafkreidef", "", sig);
        assertEq(da.lastBatchHeight(), 2);
    }

    /* a commitment and a posted batch share one chain: a reader recomputes it the same way */
    function test_ChainHash_MixedSubmitAndCommit() public {
        vm.startPrank(sequencer);
        da.submitBatch(instanceId, 1, "one");
        da.commitBatch(instanceId, 2, keccak256("two"), "ref2", "");
        da.submitBatch(instanceId, 3, "three");
        vm.stopPrank();
        bytes32 h = chain(bytes32(0), 1, "one");
        h = chain(h, 2, "two");
        h = chain(h, 3, "three");
        assertEq(da.chainHash(), h);
    }

    function test_SubmitBatch_WrongInstance() public {
        bytes32 wrongInstance = keccak256("wrong");

        vm.prank(sequencer);
        vm.expectRevert("Wrong instance");
        da.submitBatch(wrongInstance, 1, "data");
    }

    function test_SubmitBatch_NotSequencer() public {
        address random = address(0x999);
        vm.prank(random);
        vm.expectRevert("Not sequencer wallet");
        da.submitBatch(instanceId, 1, "data");
    }

    function test_SubmitBatch_NonSequential() public {
        vm.prank(sequencer);
        vm.expectRevert("Non-sequential batch");
        da.submitBatch(instanceId, 5, "data");
    }

    function test_SubmitBatch_LeaseExpired() public {
        // Fast forward past lease
        vm.warp(block.timestamp + 6 minutes);

        vm.prank(sequencer);
        vm.expectRevert("Lease expired");
        da.submitBatch(instanceId, 1, "data");
    }

    function test_GetDAState() public {
        vm.startPrank(sequencer);
        da.submitBatch(instanceId, 1, "batch1");
        da.submitBatch(instanceId, 2, "batch2");
        vm.stopPrank();

        (uint256 lastHeight, uint256 total, bytes32 ch) = da.getDAState();
        assertEq(lastHeight, 2);
        assertEq(total, 2);
        assertEq(ch, chain(chain(bytes32(0), 1, "batch1"), 2, "batch2"));
    }

    /* ============ The coordinator's signed forms (#126) ============ */

    function test_ClaimSequencerFor_RelayWithNonce() public {
        // the lease of the setup is released first: a signed claim takes a free lease
        vm.prank(sequencer);
        hpplite.releaseLease(instanceId);
        assertEq(hpplite.leaseNonce(), 0);
        bytes memory sig = sign(sequencerKey, hpplite.leaseDigest("claim", instanceId2, 0));
        vm.prank(address(0xBEEF));
        hpplite.claimSequencerFor(instanceId2, 0, sig);
        assertEq(hpplite.sequencerWallet(), sequencer);
        assertEq(hpplite.sequencerInstance(), instanceId2);
        assertTrue(hpplite.isLeaseActive());
        assertEq(hpplite.leaseNonce(), 1);

        // the signature is spent
        vm.prank(sequencer);
        hpplite.releaseLease(instanceId2);
        vm.expectRevert("Wrong nonce");
        hpplite.claimSequencerFor(instanceId2, 0, sig);

        // a stranger's signature is no claim
        bytes memory other = sign(0x2, hpplite.leaseDigest("claim", instanceId2, 1));
        vm.expectRevert("Not the sequencer");
        hpplite.claimSequencerFor(instanceId2, 1, other);
    }

    function test_RenewAndReleaseLeaseFor() public {
        uint256 expiry = hpplite.leaseExpiry();
        vm.warp(block.timestamp + 2 minutes);
        bytes memory renew = sign(sequencerKey, hpplite.leaseDigest("renew", instanceId, 0));
        vm.prank(address(0xBEEF));
        hpplite.renewLeaseFor(instanceId, 0, renew);
        assertGt(hpplite.leaseExpiry(), expiry);

        // a renew signature does not release
        bytes memory renew2 = sign(sequencerKey, hpplite.leaseDigest("renew", instanceId, 1));
        vm.expectRevert("Not sequencer wallet");          // the digest of "release" recovers another signer
        hpplite.releaseLeaseFor(instanceId, 1, renew2);

        bytes memory release = sign(sequencerKey, hpplite.leaseDigest("release", instanceId, 1));
        vm.prank(address(0xBEEF));
        hpplite.releaseLeaseFor(instanceId, 1, release);
        assertFalse(hpplite.isLeaseActive());
        assertEq(hpplite.leaseNonce(), 2);
    }

    function test_SubmitCheckpointFor() public {
        vm.startPrank(sequencer);
        da.submitBatch(instanceId, 1, "b1");
        da.submitBatch(instanceId, 2, "b2");
        vm.stopPrank();
        bytes32 root = keccak256("root");
        bytes memory sig = sign(sequencerKey, hpplite.checkpointDigest(instanceId, 1, 2, root));
        vm.prank(address(0xBEEF));
        hpplite.submitCheckpointFor(instanceId, 1, 2, root, sig);
        assertEq(hpplite.lastCheckpointHeight(), 2);
        assertEq(hpplite.lastStateRoot(), root);

        // the same range again is not the next one
        vm.expectRevert("Non-sequential");
        hpplite.submitCheckpointFor(instanceId, 1, 2, root, sig);
        // a signature for another root is another signer
        bytes memory sig2 = sign(sequencerKey, hpplite.checkpointDigest(instanceId, 3, 3, root));
        vm.expectRevert("Not the sequencer");
        hpplite.submitCheckpointFor(instanceId, 3, 3, keccak256("other"), sig2);
    }
}

contract HPPLiteFactoryTest is Test {
    HPPLiteFactory public factory;

    function setUp() public {
        factory = new HPPLiteFactory();
    }

    function test_CreateRollup() public {
        address rollup = factory.createRollup(100);

        assertTrue(rollup != address(0));
        assertEq(factory.getRollup(address(this)), rollup);
        assertTrue(factory.hasRollup(address(this)));
    }

    function test_GetOrCreateRollup() public {
        // First call creates
        address rollup1 = factory.getOrCreateRollup();
        assertTrue(rollup1 != address(0));

        // Second call returns existing
        address rollup2 = factory.getOrCreateRollup();
        assertEq(rollup1, rollup2);
    }

    function test_FactoryCreatesLinkedContracts() public {
        address rollup = factory.createRollup(50);
        address da = factory.getDA(rollup);

        HPPLite hpplite = HPPLite(rollup);
        HPPLiteDA daContract = HPPLiteDA(da);

        // Verify linkage
        assertEq(hpplite.daContract(), da);
        assertEq(address(daContract.coordinator()), rollup);
    }

    function test_OwnershipTransferred() public {
        address rollup = factory.createRollup(100);
        address da = factory.getDA(rollup);

        assertEq(HPPLite(rollup).owner(), address(this));
        assertEq(HPPLiteDA(da).owner(), address(this));
    }
}
