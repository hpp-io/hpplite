// SPDX-License-Identifier: MIT
pragma solidity ^0.8.19;

import "forge-std/Test.sol";
import "../src/HPPLiteFleet.sol";

/// The shared rollup (#297, doc/fleet-protocol.md): digests and the tree against operators/conformance/vectors.json
/// (section fleet), admission, leases and epochs, roots, retirement
contract HPPLiteFleetTest is Test {
    address constant VECTOR_ROLLUP = 0xf1f1f1F1f1f1F1F1f1F1f1F1F1F1F1f1F1f1f1F1;
    uint256 constant OWNER_PK = 0xA11CE;
    uint256 constant ADMITTER_PK = 0xAD;
    uint256 constant ANCHOR_PK = 0xA0;
    uint256 constant SEQ_PK = 0x5E9;
    uint256 constant DBOWNER_PK = 0xD0;
    uint256 constant STRANGER_PK = 0xBAD;
    HPPLiteFleet f;
    bytes32 constant DB = keccak256("a database");

    function setUp() public {
        f = new HPPLiteFleet(vm.addr(OWNER_PK), vm.addr(ANCHOR_PK), 7 days);
        vm.prank(vm.addr(OWNER_PK));
        f.setRoles(vm.addr(ADMITTER_PK), vm.addr(ANCHOR_PK), 7 days);
        vm.warp(1_800_000_000);
    }

    function sig(uint256 pk, bytes32 d) internal pure returns (bytes memory) {
        (uint8 v, bytes32 r, bytes32 s) = vm.sign(pk, d);
        return abi.encodePacked(r, s, v);
    }

    function admit(uint256 pk, uint256 issuedAt) internal { admit(pk, issuedAt, ""); }
    function admit(uint256 pk, uint256 issuedAt, bytes memory reverts) internal {
        bytes memory s = sig(pk, f.admitDigest(DB, vm.addr(DBOWNER_PK), vm.addr(SEQ_PK), issuedAt));
        if (reverts.length > 0) vm.expectRevert(reverts);
        f.admitFor(DB, vm.addr(DBOWNER_PK), vm.addr(SEQ_PK), issuedAt, s, address(0), 0, bytes32(0), bytes32(0));
    }

    function claim(bytes32 instance) internal { claim(instance, ""); }
    function claim(bytes32 instance, bytes memory reverts) internal {
        uint256 nonce = f.db(DB).claimNonce;
        uint256 deadline = block.timestamp + 60;
        bytes memory s = sig(SEQ_PK, f.claimDigest(DB, instance, nonce, deadline));
        if (reverts.length > 0) vm.expectRevert(reverts);
        f.claimFor(DB, instance, nonce, deadline, s);
    }

    function root(uint256 seq) internal {
        uint256 deadline = block.timestamp + 60;
        bytes32 r = keccak256(abi.encode("root", seq));
        bytes32 m = keccak256(abi.encode("manifest", seq));
        f.postRootFor(seq, r, m, deadline, sig(ANCHOR_PK, f.rootDigest(seq, r, m, deadline)));
    }

    function test_DigestsAndTreeAreTheVectors() public {
        vm.chainId(181228);
        deployCodeTo("HPPLiteFleet.sol", abi.encode(address(1), address(2), uint64(0)), VECTOR_ROLLUP);
        HPPLiteFleet g = HPPLiteFleet(VECTOR_ROLLUP);
        assertEq(g.headDigest(bytes32(0x0d0046e214cb0216941a015673fa828c60a42d139b75e4ad108ae5d5a348030e), bytes32(0x1fe7fbe32f2e527c0da96fe3212cc3bae9ca124c8c3acc793733702b09564abd), 1, 5, bytes32(0x825589fb316a16dbe3adb78311e2eb817072ef52dabd4c6f15065d41e2c75e7f), bytes32(0x892d5bd6622e103a7459e820912fd29738eb7197c33683b94695d3591a1e2672), bytes32(0x6aa1c15c2a69ce65c84ab680d41a88ca34647fa3b8bd3f7297142e7bc007a2b7), 1),
                 bytes32(0x801f0129092066e5863a08439ea29bd7e36e8849ed1ca991b758e79205f68e84), "Head");
        assertEq(g.admitDigest(bytes32(0x0d0046e214cb0216941a015673fa828c60a42d139b75e4ad108ae5d5a348030e), 0x1a642f0E3c3aF545E7AcBD38b07251B3990914F1, 0x5050A4F4b3f9338C3472dcC01A87C76A144b3c9c, 1800000000), bytes32(0x166a8e971bcb65f4275398ffbb883e3ededa187a00dd7a794fa5da127887e71a), "Admit");
        assertEq(g.retireDigest(bytes32(0xdc4fb9e0ec665f4dfce66a84073b1f0514c146d33659b188f0b1dadbce45995d), 1800000001), bytes32(0xdd7bcc3b8b855ffc9f29d003d99e75ccbd478cf919747b964670f851af1c1075), "Retire");
        assertEq(g.rootDigest(7, bytes32(0xa24e7097511cbf69ba9b33d4171578f371645f33ca180f2ba89eb4a460f53c6e), bytes32(0x6d9f53466d05b337ec71e9240f9180bf3da5c63eac6b0708f32174b8556b6ca5), 1800000002), bytes32(0xdcbb5c14cbe0dd5d7206420f60a43adb396f9534f306cadf1a564f80e046e62d), "Root");
        assertEq(g.dbIdOf(0x1a642f0E3c3aF545E7AcBD38b07251B3990914F1, "notes"), bytes32(0x0d0046e214cb0216941a015673fa828c60a42d139b75e4ad108ae5d5a348030e), "dbId");
        assertEq(g.leafOf(bytes32(0x0d0046e214cb0216941a015673fa828c60a42d139b75e4ad108ae5d5a348030e), 0x1a642f0E3c3aF545E7AcBD38b07251B3990914F1, bytes32(0x55fd8d795c5b9d2a906e1540ade043a6fb75de555aeb3800c4dd8cf3079b5dda), bytes32(0x801f0129092066e5863a08439ea29bd7e36e8849ed1ca991b758e79205f68e84)), bytes32(0xf7561cd2a3d4b29f39f14e52351c85ab3cca5b92ef69b0db2f15c7d691e55c76), "leaf");
        bytes32[] memory path = new bytes32[](3);
        bool[] memory left = new bool[](3);
        path[0] = bytes32(0xf7561cd2a3d4b29f39f14e52351c85ab3cca5b92ef69b0db2f15c7d691e55c76); left[0] = true;
        path[1] = bytes32(0x7248d45cac05f9e0b91e5c6969677b4c036708f01eb61a9ae9c1c34fed437b42); left[1] = false;
        path[2] = bytes32(0x770912c321a46a172c42f5dae8d1777d47cdbafbbe409f0ca25878bbee4f0540); left[2] = false;
        assertEq(g.foldPath(bytes32(0x888cb1ac4df3eeabfae289fe77d30a0bf6c6085681cee9dbf84695de7d31132d), path, left), bytes32(0x20d417f125774f98833b6526a716ac8116dc85e977d1001992d295f1f3874800), "the odd last leaf's path to the root of five");
    }

    function test_AdmissionIsTheFleets() public {
        admit(STRANGER_PK, 1, "Not admitted by the fleet");
        admit(ADMITTER_PK, 1);                                   // the admitter's word, carried by anyone (a relay)
        HPPLiteFleet.Db memory d = f.db(DB);
        assertEq(d.owner, vm.addr(DBOWNER_PK));
        assertEq(d.sequencer, vm.addr(SEQ_PK));
        admit(OWNER_PK, 2, "Already in the fleet");
    }

    function test_LeaseAndEpoch() public {
        admit(ADMITTER_PK, 1);
        claim(bytes32("host-a"));
        HPPLiteFleet.Db memory d = f.db(DB);
        assertEq(d.epoch, 1);
        assertEq(d.epochSince, 1);
        claim(bytes32("host-a"));                                // a renewal: the epoch stays
        assertEq(f.db(DB).epoch, 1);
        claim(bytes32("host-b"), "Lease held by another instance");
        root(1);
        vm.warp(block.timestamp + 7 days);                       // the lease ran out: another host takes it, the epoch moves
        claim(bytes32("host-b"));
        d = f.db(DB);
        assertEq(d.epoch, 2);
        assertEq(d.epochSince, 2);
        vm.prank(vm.addr(DBOWNER_PK));
        f.forceExpire(DB);                                       // a host lost: the owner moves the epoch at once
        assertEq(f.db(DB).epoch, 3);
        vm.prank(vm.addr(STRANGER_PK));
        vm.expectRevert(bytes("Not the database's owner"));
        f.forceExpire(DB);
        uint256 deadline = block.timestamp + 60;
        uint256 nonce = f.db(DB).claimNonce;
        bytes memory bad = sig(STRANGER_PK, f.claimDigest(DB, bytes32("host-c"), nonce, deadline));
        vm.expectRevert(bytes("Not the database's sequencer"));
        f.claimFor(DB, bytes32("host-c"), nonce, deadline, bad);
    }

    function test_RootsByTheAnchorInSequence() public {
        root(1);
        uint256 deadline = block.timestamp + 60;
        bytes32 r = keccak256("r");
        bytes32 m = keccak256("m");
        bytes memory s3 = sig(ANCHOR_PK, f.rootDigest(3, r, m, deadline));
        bytes memory bad = sig(STRANGER_PK, f.rootDigest(2, r, m, deadline));
        vm.expectRevert(bytes("Not the next root"));
        f.postRootFor(3, r, m, deadline, s3);
        vm.expectRevert(bytes("Not the fleet's anchor"));
        f.postRootFor(2, r, m, deadline, bad);
        bad = sig(ANCHOR_PK, f.rootDigest(2, r, m, deadline));     // (reused: the stack is short)
        f.postRootFor(2, r, m, deadline, bad);
        assertEq(f.rootSeq(), 2);
        assertEq(f.roots(2), r);
        assertEq(f.manifests(2), m);
    }

    function test_RetiredIsGoneUntilAdmittedAgain() public {
        admit(ADMITTER_PK, 1);
        claim(bytes32("host-a"));
        uint256 t = 2;
        bytes memory badRetire = sig(STRANGER_PK, f.retireDigest(DB, t));
        vm.expectRevert(bytes("Not the database's owner"));
        f.retireFor(DB, t, badRetire);
        f.retireFor(DB, t, sig(DBOWNER_PK, f.retireDigest(DB, t)));
        assertTrue(f.db(DB).retired);
        uint256 deadline = block.timestamp + 60;
        bytes memory s = sig(SEQ_PK, f.claimDigest(DB, bytes32("host-a"), 1, deadline));
        vm.expectRevert(bytes("No such database"));
        f.claimFor(DB, bytes32("host-a"), 1, deadline, s);       // retired: no claim without a new Admit
        admit(ADMITTER_PK, 1, "Admit not newer");
        uint64 before = f.db(DB).epoch;
        admit(ADMITTER_PK, 3);                                   // admitted again, later: epochs continue, never reused
        assertEq(f.db(DB).epoch, before);
        assertFalse(f.db(DB).retired);
    }

    function test_MigratedFromAndPromoted() public {
        f.admitFor(DB, vm.addr(DBOWNER_PK), vm.addr(SEQ_PK), 1, sig(ADMITTER_PK, f.admitDigest(DB, vm.addr(DBOWNER_PK), vm.addr(SEQ_PK), 1)),
                   address(0xDED1), 42, keccak256("chain at 42"), keccak256("root at 42"));
        HPPLiteFleet.Db memory d = f.db(DB);
        assertEq(d.migratedFrom, address(0xDED1));
        assertEq(d.genesisHeight, 42);
        vm.prank(vm.addr(DBOWNER_PK));
        f.promote(DB, address(0xDED2));
        d = f.db(DB);
        assertEq(d.promotedTo, address(0xDED2));
        assertTrue(d.retired);
    }
}
