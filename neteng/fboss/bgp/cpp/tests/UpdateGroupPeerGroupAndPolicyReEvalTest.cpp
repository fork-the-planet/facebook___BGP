/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Unit tests for update group peer, group, and policy re-evaluation.
 *
 * Test matrix:
 * https://docs.google.com/document/d/1XLc3-u0Wx7jTivHVz0tJd-PJ4NKtlPixyitfhD069Iw
 */

#define PeerManager_TEST_FRIENDS friend class UpdateGroupPolicyReEvalUTBase;

#define AdjRib_TEST_FRIENDS friend class UpdateGroupPolicyReEvalUTBase;

#define AdjRibOutGroup_TEST_FRIENDS             \
  FRIEND_TEST(SplitToGroup, CopiesGroupFields); \
  FRIEND_TEST(SplitToGroup, MovesJoinedRunningPeer);

#include "neteng/fboss/bgp/cpp/tests/UpdateGroupPolicyReEvalUTCommon.h"

namespace facebook::bgp {

class UpdateGroupPeerGroupAndPolicyReEvalTest
    : public UpdateGroupPolicyReEvalUTBase {};

TEST_F(UpdateGroupPeerGroupAndPolicyReEvalTest, FixtureSetup_KeyAndState) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto& evb = ctx.peerMgr->getEventBase();

  evb.runInEventBaseThreadAndWait([&]() {
    auto peerId0 = makePeerId(0);
    auto peerId1 = makePeerId(1);
    auto& adjRib0 = ctx.adjRibs.at(peerId0);
    auto& adjRib1 = ctx.adjRibs.at(peerId1);

    auto group = adjRib0->getUpdateGroup();
    ASSERT_NE(group, nullptr);
    EXPECT_EQ(adjRib1->getUpdateGroup().get(), group.get());

    const auto& groupKey = group->getGroupKey();
    EXPECT_EQ(groupKey.egressPolicyName, kPNameMatchNoAdvtDeny);
    EXPECT_FALSE(groupKey.peerOverride);
    EXPECT_EQ(groupKey.peerGroupName, "PEERGROUP_A");

    const auto& peerKey0 = adjRib0->getUpdateGroupKey();
    EXPECT_EQ(peerKey0.egressPolicyName, kPNameMatchNoAdvtDeny);
    EXPECT_FALSE(peerKey0.peerOverride);
    EXPECT_EQ(peerKey0, groupKey);

    const auto& peerKey1 = adjRib1->getUpdateGroupKey();
    EXPECT_EQ(peerKey1, groupKey);

    EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
    EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::JOINED_RUNNING);

    EXPECT_EQ(group->getMemberCount(), 2);
  });

  auto peerId0 = makePeerId(0);
  updatePeerEgressPolicyOnEvb(ctx, peerId0, kPNameMatchModifyAppend);

  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib0 = ctx.adjRibs.at(peerId0);
    adjRib0->buildAndSetUpdateGroupKey();
    const auto& updatedKey = adjRib0->getUpdateGroupKey();
    EXPECT_EQ(updatedKey.egressPolicyName, kPNameMatchModifyAppend);
    EXPECT_TRUE(updatedKey.peerOverride);
  });

  /*
   * Clear peer0's per-peer override so its update group key matches the group
   * it is still a member of. The override was only set for the assertion above;
   * the peer was never moved to a matching group, so leaving the key diverged
   * makes shutdown's maybeDestroyUpdateGroup key off peer0's stale override
   * key, never find the group, and leak it. unsetPeersPolicy (not
   * setPeersPolicy) is required to actually remove the override and clear
   * peerOverride.
   */
  unsetPeerEgressPolicyOnEvb(ctx, peerId0);
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib0 = ctx.adjRibs.at(peerId0);
    adjRib0->buildAndSetUpdateGroupKey();
    EXPECT_FALSE(adjRib0->getUpdateGroupKey().peerOverride);
  });

  tearDown(ctx);
}

TEST_F(
    UpdateGroupPeerGroupAndPolicyReEvalTest,
    TriggerDetachedBlocked_FrequencyDetach) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);
  auto peerId1 = makePeerId(1);

  triggerDetachedBlockedFromJoinedOnEvb(ctx, peerId0, true);

  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib0 = ctx.adjRibs.at(peerId0);
    auto& adjRib1 = ctx.adjRibs.at(peerId1);
    auto group = adjRib1->getUpdateGroup();

    EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
    EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::JOINED_RUNNING);
    EXPECT_EQ(group->getNumInSyncPeers(), 1);
  });

  tearDown(ctx);
}

TEST_F(UpdateGroupPeerGroupAndPolicyReEvalTest, PeerDownPeerUp) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);
  auto& evb = ctx.peerMgr->getEventBase();

  auto originalGroup = ctx.adjRibs.at(peerId0)->getUpdateGroup();

  evb.runInEventBaseThreadAndWait([&]() {
    EXPECT_EQ(
        ctx.adjRibs.at(peerId0)->getPeerState(),
        PeerUpdateState::JOINED_RUNNING);
  });

  triggerPeerDownOnEvb(ctx, peerId0);

  expectEventualStateOnEvb(ctx, peerId0, PeerUpdateState::DOWN);

  triggerPeerUpOnEvb(ctx, peerId0);

  WITH_RETRIES({
    auto state = folly::via(&evb, [&]() {
                   return ctx.adjRibs.at(peerId0)->getPeerState();
                 }).get();
    EXPECT_EVENTUALLY_NE(state, PeerUpdateState::DOWN);
  });

  // Verify peer rejoined the same update group
  evb.runInEventBaseThreadAndWait([&]() {
    auto newGroup = ctx.adjRibs.at(peerId0)->getUpdateGroup();
    EXPECT_EQ(
        UpdateGroupKey::toString(newGroup->getGroupKey()),
        UpdateGroupKey::toString(originalGroup->getGroupKey()));
  });

  tearDown(ctx);
}

TEST_F(UpdateGroupPeerGroupAndPolicyReEvalTest, DetachedReadyToJoin_DFP) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);
  auto peerId1 = makePeerId(1);

  triggerPeerDetachedReadyToJoin(ctx, peerId1, /*isDFP=*/true);

  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    EXPECT_EQ(
        ctx.adjRibs.at(peerId1)->getPeerState(),
        PeerUpdateState::DETACHED_READY_TO_JOIN);
    EXPECT_EQ(
        ctx.adjRibs.at(peerId0)->getPeerState(),
        PeerUpdateState::JOINED_BLOCKED);
  });

  tearDown(ctx);
}

TEST_F(UpdateGroupPeerGroupAndPolicyReEvalTest, DetachedReadyToJoin_DSP) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);
  auto peerId1 = makePeerId(1);

  triggerPeerDetachedReadyToJoin(ctx, peerId1, /*isDFP=*/false);

  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    EXPECT_EQ(
        ctx.adjRibs.at(peerId1)->getPeerState(),
        PeerUpdateState::DETACHED_READY_TO_JOIN);
    EXPECT_EQ(
        ctx.adjRibs.at(peerId0)->getPeerState(),
        PeerUpdateState::JOINED_BLOCKED);
  });

  tearDown(ctx);
}

TEST_F(UpdateGroupPeerGroupAndPolicyReEvalTest, DetachedBlocked_FromDetached) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);

  triggerDetachedBlockedFromDetachedOnEvb(ctx, peerId0);

  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    EXPECT_EQ(
        ctx.adjRibs.at(peerId0)->getPeerState(),
        PeerUpdateState::DETACHED_BLOCKED);
  });

  tearDown(ctx);
}

TEST_F(UpdateGroupPeerGroupAndPolicyReEvalTest, DetachedInitDump) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);

  auto peerId0 = makePeerId(0);
  expectEventualStateOnEvb(ctx, peerId0, PeerUpdateState::JOINED_RUNNING);

  auto didPeerId = triggerDetachedInitDump(ctx, 0);

  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    EXPECT_EQ(
        ctx.adjRibs.at(didPeerId)->getPeerState(),
        PeerUpdateState::DETACHED_INIT_DUMP);
  });

  tearDown(ctx);
}

TEST_F(
    UpdateGroupPeerGroupAndPolicyReEvalTest,
    GroupWaitingBlockedByJoinedPeer) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);
  auto peerId1 = makePeerId(1);

  blockGroupViaPeerOnEvb(ctx, peerId1);
  publishRouteUpdates(ctx, /*isInitialDump=*/false);

  auto& evb = ctx.peerMgr->getEventBase();

  WITH_RETRIES({
    auto state = folly::via(&evb, [&]() {
                   return ctx.adjRibs.at(peerId0)->getUpdateGroup()->getState();
                 }).get();
    EXPECT_EVENTUALLY_EQ(state, UpdateGroupState::WAITING);
  });

  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib0 = ctx.adjRibs.at(peerId0);
    auto& adjRib1 = ctx.adjRibs.at(peerId1);

    EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
    EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::JOINED_BLOCKED);

    auto group = adjRib0->getUpdateGroup();
    EXPECT_EQ(group->getState(), UpdateGroupState::WAITING);
    EXPECT_EQ(group->getMemberCount(), 2);
  });

  tearDown(ctx);
}

/*
 * splitToNewGroup re-homes a subset of peers from one update group into a new
 * group, cloning the group's operational state onto it. There is no production
 * caller yet (the per-peer split path is a TODO(6.6) stub), so each test stands
 * up a real group and peers via the fixture, drives a peer into a genuine
 * state, then calls splitToNewGroup directly and verifies the re-home.
 */
class SplitToGroup : public UpdateGroupPolicyReEvalUTBase {};

/*
 * Smoke test that just prints serializeSharedRibOutTreesGroupKeyNormalized's
 * output so the format can be eyeballed. The realistic fixture is non-add-path,
 * so PathTree is empty and the entries land in LiteTree.
 */
TEST_F(SplitToGroup, SerializeRibOutTreesForOwner_PrintsSnapshot) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId1 = makePeerId(1);
  expectEventualStateOnEvb(ctx, peerId1, PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  std::string snapshot;
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(peerId1);
    snapshot = serializeSharedRibOutTreesGroupKeyNormalized(
        adjRib->getUpdateGroup(), adjRib->getPeerOwnerKey());
  });

  XLOGF(
      INFO,
      "serializeSharedRibOutTreesGroupKeyNormalized(peer1):\n{}",
      snapshot);
  EXPECT_FALSE(snapshot.empty());

  tearDown(ctx);
}

/*
 * splitToNewGroup copies the old group's operational state, flags, group-owned
 * RIB-OUT, change-list tracker, and consumer position onto the new group, and
 * leaves the old group's copies unmodified.
 */
TEST_F(SplitToGroup, CopiesGroupFields) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);
  auto& evb = ctx.peerMgr->getEventBase();

  /*
   * Route 0 (100.0.0.0/24) is advertised and, with both peers in sync, owned by
   * the group, so it exercises the group-owned RIB-OUT copy.
   */
  const folly::CIDRNetwork sharedPrefix{folly::IPAddress("100.0.0.0"), 24};

  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib0 = ctx.adjRibs.at(peerId0);
    auto sourceGroup = adjRib0->getUpdateGroup();
    ASSERT_NE(sourceGroup, nullptr);
    ASSERT_NE(sourceGroup->getChangeListConsumer(), nullptr);
    ASSERT_NE(
        sourceGroup->getFromLiteTree(
            sourceGroup->LiteTree_,
            sharedPrefix,
            sourceGroup->getGroupOwnerKey()),
        nullptr);

    /*
     * Distinctive (test-chosen) values on the source group so the post-split
     * target-vs-source comparison is meaningful.
     */
    sourceGroup->setState(UpdateGroupState::READY);
    sourceGroup->setLastSeenRibVersion(12345);
    sourceGroup->mraiInterval_ = 30000;
    sourceGroup->enableRibAllocatedPathId_ = true;
    sourceGroup->egressEoRPendingV4_ = true;
    sourceGroup->egressEoRPendingV6_ = true;
    sourceGroup->initialDumpCompletionTimeMs_ = int64_t{777};
    if (!sourceGroup->peeringParams_.has_value()) {
      sourceGroup->peeringParams_ = PeeringParams();
    }
    sourceGroup->peeringParams_->globalAs = 64512;
    UpdateGroupConfig cfg;
    cfg.enableSerializeGroupPdu = true;
    cfg.slowPeerBlockCountThreshold = 42;
    sourceGroup->setUpdateGroupConfigForTesting(cfg);

    auto* sourceMarker = sourceGroup->getChangeListConsumer()->getMarker();

    /*
     * splitToNewGroup requires the caller to construct the target as a proper
     * update group (enableUpdateGroup + the source's key);
     * copyGroupFieldsToNew- Group deliberately does not copy identity, so a
     * faithful clone must match what UpdateGroupManager::findOrCreateGroup
     * builds.
     */
    auto targetGroup = std::make_shared<AdjRibOutGroup>(
        evb,
        "split_target",
        sourceGroup->getGroupId() + 1,
        /*enableUpdateGroup=*/true,
        sourceGroup->getGroupKey());
    sourceGroup->splitToNewGroup(targetGroup, {adjRib0});

    /*
     * Every operational field on the new group matches the old group's: the
     * distinctive values set above prove they were copied, and the old group
     * still holding them proves the split left it unmodified.
     */
    EXPECT_EQ(targetGroup->getState(), sourceGroup->getState());
    EXPECT_EQ(
        targetGroup->getLastSeenRibVersion(),
        sourceGroup->getLastSeenRibVersion());
    EXPECT_EQ(targetGroup->mraiInterval_, sourceGroup->mraiInterval_);
    EXPECT_EQ(
        targetGroup->enableRibAllocatedPathId_,
        sourceGroup->enableRibAllocatedPathId_);
    EXPECT_EQ(
        targetGroup->egressEoRPendingV4_, sourceGroup->egressEoRPendingV4_);
    EXPECT_EQ(
        targetGroup->egressEoRPendingV6_, sourceGroup->egressEoRPendingV6_);
    EXPECT_EQ(
        targetGroup->getInitialDumpCompletionTimeMs(),
        sourceGroup->getInitialDumpCompletionTimeMs());
    ASSERT_TRUE(targetGroup->peeringParams_.has_value());
    EXPECT_EQ(
        targetGroup->peeringParams_->globalAs,
        sourceGroup->peeringParams_->globalAs);
    EXPECT_EQ(
        targetGroup->updateGroupConfig_.enableSerializeGroupPdu,
        sourceGroup->updateGroupConfig_.enableSerializeGroupPdu);
    EXPECT_EQ(
        targetGroup->updateGroupConfig_.slowPeerBlockCountThreshold,
        sourceGroup->updateGroupConfig_.slowPeerBlockCountThreshold);
    // The change list tracker is shared, not duplicated.
    EXPECT_EQ(
        targetGroup->getChangeListTracker(),
        sourceGroup->getChangeListTracker());
    // The new group's consumer joins at the old group's (unchanged) marker.
    ASSERT_NE(targetGroup->getChangeListConsumer(), nullptr);
    EXPECT_EQ(targetGroup->getChangeListConsumer()->getMarker(), sourceMarker);
    EXPECT_EQ(sourceGroup->getChangeListConsumer()->getMarker(), sourceMarker);
    // The group-owned RIB-OUT is copied to the new group (kept in the old).
    EXPECT_NE(
        targetGroup->getFromLiteTree(
            targetGroup->LiteTree_,
            sharedPrefix,
            targetGroup->getGroupOwnerKey()),
        nullptr);
    EXPECT_NE(
        sourceGroup->getFromLiteTree(
            sourceGroup->LiteTree_,
            sharedPrefix,
            sourceGroup->getGroupOwnerKey()),
        nullptr);

    /*
     * Break the peer<->group and group<->consumer cycles splitToNewGroup
     * created on the unmanaged target group so it (and its copied entries,
     * which reference the AdjRibPolicyCache singleton) is destroyed at end of
     * test instead of leaking past folly Singleton teardown.
     */
    targetGroup->unregisterPeer(adjRib0);
    adjRib0->setUpdateGroup(nullptr);
    if (auto consumer = targetGroup->getChangeListConsumer()) {
      consumer->resetBitmap();
      consumer->terminate();
      consumer->deregisterFromTracker();
    }
    targetGroup->resetChangeListConsumer();
  });

  tearDown(ctx);
}

/*
 * A JOINED_RUNNING peer is in sync: the split keeps it in sync in the new group
 * with its RIB-OUT preserved entry-for-entry.
 */
TEST_F(SplitToGroup, MovesJoinedRunningPeer) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId1 = makePeerId(1);
  expectEventualStateOnEvb(ctx, peerId1, PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  std::string ribOutBefore, ribOutAfter;
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(peerId1);
    auto sourceGroup = adjRib->getUpdateGroup();
    auto ownerKey = adjRib->getPeerOwnerKey();
    ribOutBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey);

    auto targetGroup = std::make_shared<AdjRibOutGroup>(
        evb,
        "split_target",
        sourceGroup->getGroupId() + 1,
        /*enableUpdateGroup=*/true,
        sourceGroup->getGroupKey());
    sourceGroup->splitToNewGroup(targetGroup, {adjRib});

    // Re-homed into the new group; peer 0 stays behind.
    EXPECT_EQ(adjRib->getUpdateGroup(), targetGroup);
    EXPECT_EQ(targetGroup->getMemberCount(), 1);
    EXPECT_EQ(sourceGroup->getMemberCount(), 1);
    // State preserved; still in sync, not detached.
    EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::JOINED_RUNNING);
    EXPECT_FALSE(adjRib->isDetachedPeer());
    EXPECT_TRUE(targetGroup->isPeerInSync(adjRib->getGroupBitPosition()));

    ribOutAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(targetGroup, ownerKey);
    /*
     * Break the peer<->group and group<->consumer cycles splitToNewGroup
     * created on the unmanaged target group so it (and its copied entries,
     * which reference the AdjRibPolicyCache singleton) is destroyed at end of
     * test instead of leaking past folly Singleton teardown.
     */
    targetGroup->unregisterPeer(adjRib);
    adjRib->setUpdateGroup(nullptr);
    if (auto consumer = targetGroup->getChangeListConsumer()) {
      consumer->resetBitmap();
      consumer->terminate();
      consumer->deregisterFromTracker();
    }
    targetGroup->resetChangeListConsumer();
  });

  EXPECT_NE(ribOutBefore, "Path{}, Lite{}");
  EXPECT_EQ(ribOutAfter, ribOutBefore);

  tearDown(ctx);
}

/*
 * A JOINED_BLOCKED peer is still an in-sync state: the split preserves the
 * blocked state, keeps it in sync, and preserves its RIB-OUT entry-for-entry.
 */
TEST_F(SplitToGroup, MovesJoinedBlockedPeer) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId1 = makePeerId(1);

  blockGroupViaPeerOnEvb(ctx, peerId1);
  publishRouteUpdates(ctx, /*isInitialDump=*/false);
  expectEventualStateOnEvb(ctx, peerId1, PeerUpdateState::JOINED_BLOCKED);

  auto& evb = ctx.peerMgr->getEventBase();
  std::string ribOutBefore, ribOutAfter;
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(peerId1);
    auto sourceGroup = adjRib->getUpdateGroup();
    auto ownerKey = adjRib->getPeerOwnerKey();
    ribOutBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey);

    auto targetGroup = std::make_shared<AdjRibOutGroup>(
        evb,
        "split_target",
        sourceGroup->getGroupId() + 1,
        /*enableUpdateGroup=*/true,
        sourceGroup->getGroupKey());
    sourceGroup->splitToNewGroup(targetGroup, {adjRib});

    EXPECT_EQ(adjRib->getUpdateGroup(), targetGroup);
    EXPECT_EQ(targetGroup->getMemberCount(), 1);
    EXPECT_EQ(sourceGroup->getMemberCount(), 1);
    EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::JOINED_BLOCKED);
    EXPECT_FALSE(adjRib->isDetachedPeer());
    EXPECT_TRUE(targetGroup->isPeerInSync(adjRib->getGroupBitPosition()));
    /*
     * The peer is JOINED_BLOCKED: in-sync AND blocked. The blocked bit must be
     * restored on the new group and cleared on the old (it has only the moved
     * peer, so hasBlockedPeers reflects exactly this peer's bit).
     */
    EXPECT_TRUE(targetGroup->hasBlockedPeers());
    EXPECT_FALSE(sourceGroup->hasBlockedPeers());

    ribOutAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(targetGroup, ownerKey);
    /*
     * Break the peer<->group and group<->consumer cycles splitToNewGroup
     * created on the unmanaged target group so it (and its copied entries,
     * which reference the AdjRibPolicyCache singleton) is destroyed at end of
     * test instead of leaking past folly Singleton teardown.
     */
    targetGroup->unregisterPeer(adjRib);
    adjRib->setUpdateGroup(nullptr);
    if (auto consumer = targetGroup->getChangeListConsumer()) {
      consumer->resetBitmap();
      consumer->terminate();
      consumer->deregisterFromTracker();
    }
    targetGroup->resetChangeListConsumer();
  });

  EXPECT_NE(ribOutBefore, "Path{}, Lite{}");
  EXPECT_EQ(ribOutAfter, ribOutBefore);

  tearDown(ctx);
}

/*
 * A DETACHED_BLOCKED peer is out of sync: the split keeps it detached and out
 * of sync in the new group, and its RIB-OUT (per-peer + shared) is preserved
 * entry-for-entry -- a detached peer keeps its detached RIB version across the
 * split, so even its per-peer entries carry over unchanged.
 */
TEST_F(SplitToGroup, MovesDetachedBlockedPeer) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId1 = makePeerId(1);

  triggerDetachedBlockedFromJoinedOnEvb(
      ctx, peerId1, /*useBlockFrequency=*/true);
  expectEventualStateOnEvb(ctx, peerId1, PeerUpdateState::DETACHED_BLOCKED);

  auto& evb = ctx.peerMgr->getEventBase();
  std::string ribOutBefore, ribOutAfter;
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(peerId1);
    auto sourceGroup = adjRib->getUpdateGroup();
    auto ownerKey = adjRib->getPeerOwnerKey();
    ribOutBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey);

    auto targetGroup = std::make_shared<AdjRibOutGroup>(
        evb,
        "split_target",
        sourceGroup->getGroupId() + 1,
        /*enableUpdateGroup=*/true,
        sourceGroup->getGroupKey());
    sourceGroup->splitToNewGroup(targetGroup, {adjRib});

    EXPECT_EQ(adjRib->getUpdateGroup(), targetGroup);
    EXPECT_EQ(targetGroup->getMemberCount(), 1);
    EXPECT_EQ(sourceGroup->getMemberCount(), 1);
    EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
    EXPECT_TRUE(adjRib->isDetachedPeer());
    EXPECT_FALSE(targetGroup->isPeerInSync(adjRib->getGroupBitPosition()));

    ribOutAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(targetGroup, ownerKey);
    /*
     * Break the peer<->group and group<->consumer cycles splitToNewGroup
     * created on the unmanaged target group so it (and its copied entries,
     * which reference the AdjRibPolicyCache singleton) is destroyed at end of
     * test instead of leaking past folly Singleton teardown.
     */
    targetGroup->unregisterPeer(adjRib);
    adjRib->setUpdateGroup(nullptr);
    if (auto consumer = targetGroup->getChangeListConsumer()) {
      consumer->resetBitmap();
      consumer->terminate();
      consumer->deregisterFromTracker();
    }
    targetGroup->resetChangeListConsumer();
  });

  EXPECT_NE(ribOutBefore, "Path{}, Lite{}");
  EXPECT_EQ(ribOutAfter, ribOutBefore);

  tearDown(ctx);
}

/*
 * A DETACHED_READY_TO_JOIN peer is out of sync: the split keeps it detached and
 * out of sync in the new group with its RIB-OUT preserved entry-for-entry.
 */
TEST_F(SplitToGroup, MovesDetachedReadyToJoinPeer) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);
  auto peerId1 = makePeerId(1);

  triggerPeerDetachedReadyToJoin(ctx, peerId1, /*isDFP=*/true);
  expectEventualStateOnEvb(
      ctx, peerId1, PeerUpdateState::DETACHED_READY_TO_JOIN);

  auto& evb = ctx.peerMgr->getEventBase();
  std::string ribOutBefore, ribOutAfter;
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(peerId1);
    auto sourceGroup = adjRib->getUpdateGroup();
    auto ownerKey = adjRib->getPeerOwnerKey();
    ribOutBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey);

    auto targetGroup = std::make_shared<AdjRibOutGroup>(
        evb,
        "split_target",
        sourceGroup->getGroupId() + 1,
        /*enableUpdateGroup=*/true,
        sourceGroup->getGroupKey());
    sourceGroup->splitToNewGroup(targetGroup, {adjRib});

    EXPECT_EQ(adjRib->getUpdateGroup(), targetGroup);
    EXPECT_EQ(targetGroup->getMemberCount(), 1);
    EXPECT_EQ(sourceGroup->getMemberCount(), 1);
    EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);
    EXPECT_TRUE(adjRib->isDetachedPeer());
    EXPECT_FALSE(targetGroup->isPeerInSync(adjRib->getGroupBitPosition()));

    ribOutAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(targetGroup, ownerKey);
    /*
     * Break the peer<->group and group<->consumer cycles splitToNewGroup
     * created on the unmanaged target group so it (and its copied entries,
     * which reference the AdjRibPolicyCache singleton) is destroyed at end of
     * test instead of leaking past folly Singleton teardown.
     */
    targetGroup->unregisterPeer(adjRib);
    adjRib->setUpdateGroup(nullptr);
    if (auto consumer = targetGroup->getChangeListConsumer()) {
      consumer->resetBitmap();
      consumer->terminate();
      consumer->deregisterFromTracker();
    }
    targetGroup->resetChangeListConsumer();
  });

  EXPECT_NE(ribOutBefore, "Path{}, Lite{}");
  EXPECT_EQ(ribOutAfter, ribOutBefore);

  tearDown(ctx);
}

/*
 * A DETACHED_INIT_DUMP peer is out of sync: the split keeps it detached and out
 * of sync in the new group. The peer's own RIB-OUT may be empty mid initial
 * dump, but the group-owned (shared) entries are copied faithfully, so its
 * serialized RIB-OUT view is preserved entry-for-entry.
 */
TEST_F(SplitToGroup, MovesDetachedInitDumpPeer) {
  auto ctx = setUp(2);
  sendInitialRibDump(ctx);

  auto peerId0 = makePeerId(0);
  expectEventualStateOnEvb(ctx, peerId0, PeerUpdateState::JOINED_RUNNING);

  /*
   * Detaches peer 0 into DETACHED_INIT_DUMP; peer 1 stays JOINED_RUNNING so the
   * source group keeps an in-sync member after the split.
   */
  auto detachedPeerId = triggerDetachedInitDump(ctx, 0);

  auto& evb = ctx.peerMgr->getEventBase();
  std::string ribOutBefore, ribOutAfter;
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(detachedPeerId);
    auto sourceGroup = adjRib->getUpdateGroup();
    auto ownerKey = adjRib->getPeerOwnerKey();
    ribOutBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey);

    auto targetGroup = std::make_shared<AdjRibOutGroup>(
        evb,
        "split_target",
        sourceGroup->getGroupId() + 1,
        /*enableUpdateGroup=*/true,
        sourceGroup->getGroupKey());
    sourceGroup->splitToNewGroup(targetGroup, {adjRib});

    EXPECT_EQ(adjRib->getUpdateGroup(), targetGroup);
    EXPECT_EQ(targetGroup->getMemberCount(), 1);
    EXPECT_EQ(sourceGroup->getMemberCount(), 1);
    EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::DETACHED_INIT_DUMP);
    EXPECT_TRUE(adjRib->isDetachedPeer());
    EXPECT_FALSE(targetGroup->isPeerInSync(adjRib->getGroupBitPosition()));

    ribOutAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(targetGroup, ownerKey);
    /*
     * Break the peer<->group and group<->consumer cycles splitToNewGroup
     * created on the unmanaged target group so it (and its copied entries,
     * which reference the AdjRibPolicyCache singleton) is destroyed at end of
     * test instead of leaking past folly Singleton teardown.
     */
    targetGroup->unregisterPeer(adjRib);
    adjRib->setUpdateGroup(nullptr);
    if (auto consumer = targetGroup->getChangeListConsumer()) {
      consumer->resetBitmap();
      consumer->terminate();
      consumer->deregisterFromTracker();
    }
    targetGroup->resetChangeListConsumer();
  });

  EXPECT_NE(ribOutBefore, "Path{}, Lite{}");
  EXPECT_EQ(ribOutAfter, ribOutBefore);

  tearDown(ctx);
}

/*
 * splitToNewGroup can move a set of peers -- a mix of in-sync and detached --
 * in one call: peer 1 (DETACHED_BLOCKED) and peer 2 (JOINED_RUNNING) are split
 * out together while peer 0 stays behind. Each moved peer is re-homed at its
 * own bit with its state, in-sync status, and RIB-OUT preserved
 * entry-for-entry, and the peer left behind is unaffected.
 */
TEST_F(SplitToGroup, MovesMultiplePeersAtOnce) {
  auto ctx = setUp(3);
  sendInitialRibDump(ctx);
  auto peerId0 = makePeerId(0);
  auto peerId1 = makePeerId(1);
  auto peerId2 = makePeerId(2);

  // Detach peer 1; peers 0 and 2 stay JOINED_RUNNING.
  triggerDetachedBlockedFromJoinedOnEvb(
      ctx, peerId1, /*useBlockFrequency=*/true);
  expectEventualStateOnEvb(ctx, peerId1, PeerUpdateState::DETACHED_BLOCKED);
  expectEventualStateOnEvb(ctx, peerId2, PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  /*
   * Naming: <peer><group><when>, e.g. peer1TargetAfter = peer 1's view of the
   * target group after the split.
   */
  std::string peer0SourceBefore, peer0SourceAfter;
  std::string peer1SourceBefore, peer1TargetAfter, peer1SourceAfter;
  std::string peer2SourceBefore, peer2TargetAfter, peer2SourceAfter;
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib0 = ctx.adjRibs.at(peerId0);
    auto& adjRib1 = ctx.adjRibs.at(peerId1);
    auto& adjRib2 = ctx.adjRibs.at(peerId2);
    auto sourceGroup = adjRib1->getUpdateGroup();
    ASSERT_EQ(adjRib0->getUpdateGroup(), sourceGroup);
    ASSERT_EQ(adjRib2->getUpdateGroup(), sourceGroup);
    auto ownerKey0 = adjRib0->getPeerOwnerKey();
    auto ownerKey1 = adjRib1->getPeerOwnerKey();
    auto ownerKey2 = adjRib2->getPeerOwnerKey();
    peer0SourceBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey0);
    peer1SourceBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey1);
    peer2SourceBefore =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey2);

    auto targetGroup = std::make_shared<AdjRibOutGroup>(
        evb,
        "split_target",
        sourceGroup->getGroupId() + 1,
        /*enableUpdateGroup=*/true,
        sourceGroup->getGroupKey());
    sourceGroup->splitToNewGroup(targetGroup, {adjRib1, adjRib2});

    // Both peers re-homed into the new group; peer 0 stays behind.
    EXPECT_EQ(adjRib1->getUpdateGroup(), targetGroup);
    EXPECT_EQ(adjRib2->getUpdateGroup(), targetGroup);
    EXPECT_EQ(adjRib0->getUpdateGroup(), sourceGroup);
    EXPECT_EQ(targetGroup->getMemberCount(), 2);
    EXPECT_EQ(sourceGroup->getMemberCount(), 1);
    // Moved peers land at distinct bits; each carries its own state and sync
    // status -- peer 1 detached/out-of-sync, peer 2 in-sync.
    EXPECT_NE(adjRib1->getGroupBitPosition(), adjRib2->getGroupBitPosition());
    EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
    EXPECT_TRUE(adjRib1->isDetachedPeer());
    EXPECT_FALSE(targetGroup->isPeerInSync(adjRib1->getGroupBitPosition()));
    EXPECT_EQ(adjRib2->getPeerState(), PeerUpdateState::JOINED_RUNNING);
    EXPECT_FALSE(adjRib2->isDetachedPeer());
    EXPECT_TRUE(targetGroup->isPeerInSync(adjRib2->getGroupBitPosition()));

    /*
     * Re-read each peer's RIB-OUT after the split: peer 0 from the source group
     * it stayed in, and peers 1 and 2 from both the source they left and their
     * new target group.
     */
    peer0SourceAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey0);
    peer1SourceAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey1);
    peer1TargetAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(targetGroup, ownerKey1);
    peer2SourceAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(sourceGroup, ownerKey2);
    peer2TargetAfter =
        serializeSharedRibOutTreesGroupKeyNormalized(targetGroup, ownerKey2);

    /*
     * Break the peer<->group and group<->consumer cycles splitToNewGroup
     * created on the unmanaged target group so it (and its copied entries,
     * which reference the AdjRibPolicyCache singleton) is destroyed at end of
     * test instead of leaking past folly Singleton teardown.
     */
    targetGroup->unregisterPeer(adjRib1);
    targetGroup->unregisterPeer(adjRib2);
    adjRib1->setUpdateGroup(nullptr);
    adjRib2->setUpdateGroup(nullptr);
    if (auto consumer = targetGroup->getChangeListConsumer()) {
      consumer->resetBitmap();
      consumer->terminate();
      consumer->deregisterFromTracker();
    }
    targetGroup->resetChangeListConsumer();
  });

  // Peer 0: in-sync (group-owned only) and untouched by the split.
  EXPECT_EQ(peer0SourceBefore.find("p_"), std::string::npos);
  EXPECT_EQ(peer0SourceAfter, peer0SourceBefore);
  // Peer 1: detached; left the source clean, RIB-OUT preserved in the target.
  EXPECT_EQ(peer1SourceAfter.find("p_"), std::string::npos);
  EXPECT_EQ(peer1TargetAfter, peer1SourceBefore);
  // Peer 2: in-sync; left the source clean, RIB-OUT preserved & group-owned.
  EXPECT_EQ(peer2SourceAfter.find("p_"), std::string::npos);
  EXPECT_EQ(peer2TargetAfter, peer2SourceBefore);
  EXPECT_EQ(peer2TargetAfter.find("p_"), std::string::npos);

  tearDown(ctx);
}

/*
 * The CancellationSource_* tests below unit-test the per-group egress-policy
 * re-evaluation cancellation source APIs on AdjRibOutGroup that back
 * PeerManager's coalescing and cancellation of scheduled group re-evaluations,
 * one API per test:
 *   - isEgressPolicyReEvaluationScheduled
 *   - getCancellationTokenForNewEgressPolicyReEvaluation
 *   - cancelEgressPolicyReEvaluation
 *   - resetEgressPolicyReEvaluationCancellationSource
 * plus one test for the "superseded re-evaluation" interaction between them.
 * This mirrors the per-AdjRib RibDumpCancellationSource_* tests in
 * PeerMgrRibTest.cpp. The source APIs are exercised on the group's EventBase
 * thread since the group is owned by the peer manager.
 */
class GroupEgressPolicyReEvalCancellation
    : public UpdateGroupPolicyReEvalUTBase {};

/*
 * isEgressPolicyReEvaluationScheduled: false with no source, true once a source
 * exists, false again after the source is cleared.
 */
TEST_F(GroupEgressPolicyReEvalCancellation, CancellationSource_IsScheduled) {
  auto ctx = setUp(2);
  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    auto group = ctx.adjRibs.at(makePeerId(0))->getUpdateGroup();
    ASSERT_NE(group, nullptr);

    EXPECT_FALSE(group->isEgressPolicyReEvaluationScheduled());

    group->getCancellationTokenForNewEgressPolicyReEvaluation();
    EXPECT_TRUE(group->isEgressPolicyReEvaluationScheduled());

    group->cancelEgressPolicyReEvaluation();
    EXPECT_FALSE(group->isEgressPolicyReEvaluationScheduled());
  });
  tearDown(ctx);
}

/*
 * getCancellationTokenForNewEgressPolicyReEvaluation: creates a source (so a
 * re-evaluation is now scheduled) and returns a live, un-cancelled token.
 */
TEST_F(GroupEgressPolicyReEvalCancellation, CancellationSource_GetToken) {
  auto ctx = setUp(2);
  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    auto group = ctx.adjRibs.at(makePeerId(0))->getUpdateGroup();
    ASSERT_NE(group, nullptr);

    EXPECT_FALSE(group->isEgressPolicyReEvaluationScheduled());

    auto token = group->getCancellationTokenForNewEgressPolicyReEvaluation();
    EXPECT_TRUE(group->isEgressPolicyReEvaluationScheduled());
    EXPECT_FALSE(token.isCancellationRequested());
  });
  tearDown(ctx);
}

/*
 * cancelEgressPolicyReEvaluation: cancels the in-flight token AND clears the
 * source. The previously handed-out token still observes the cancellation (the
 * underlying state outlives the source). A second cancel is a no-op.
 */
TEST_F(GroupEgressPolicyReEvalCancellation, CancellationSource_Cancel) {
  auto ctx = setUp(2);
  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    auto group = ctx.adjRibs.at(makePeerId(0))->getUpdateGroup();
    ASSERT_NE(group, nullptr);

    auto token = group->getCancellationTokenForNewEgressPolicyReEvaluation();
    EXPECT_TRUE(group->isEgressPolicyReEvaluationScheduled());

    group->cancelEgressPolicyReEvaluation();
    EXPECT_FALSE(group->isEgressPolicyReEvaluationScheduled());
    EXPECT_TRUE(token.isCancellationRequested());

    // cancelEgressPolicyReEvaluation is idempotent when no source exists.
    group->cancelEgressPolicyReEvaluation();
    EXPECT_FALSE(group->isEgressPolicyReEvaluationScheduled());
  });
  tearDown(ctx);
}

/*
 * resetEgressPolicyReEvaluationCancellationSource: models normal completion.
 * Clears the source (so a future re-evaluation can be scheduled) WITHOUT
 * cancelling, so a token handed out before the reset stays un-cancelled.
 */
TEST_F(GroupEgressPolicyReEvalCancellation, CancellationSource_Reset) {
  auto ctx = setUp(2);
  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    auto group = ctx.adjRibs.at(makePeerId(0))->getUpdateGroup();
    ASSERT_NE(group, nullptr);

    auto token = group->getCancellationTokenForNewEgressPolicyReEvaluation();
    EXPECT_TRUE(group->isEgressPolicyReEvaluationScheduled());

    group->resetEgressPolicyReEvaluationCancellationSource();
    EXPECT_FALSE(group->isEgressPolicyReEvaluationScheduled());
    EXPECT_FALSE(token.isCancellationRequested());
  });
  tearDown(ctx);
}

/*
 * "Don't reset the wrong source": when a re-evaluation is superseded, the old
 * token is cancelled and a fresh source is created for the replacement. The old
 * task's completion guard (token.isCancellationRequested()) is what prevents it
 * from resetting the new source. Replicate that guard here and verify the new
 * source survives.
 */
TEST_F(
    GroupEgressPolicyReEvalCancellation,
    CancellationSource_SupersededKeepsNewerSource) {
  auto ctx = setUp(2);
  auto& evb = ctx.peerMgr->getEventBase();
  evb.runInEventBaseThreadAndWait([&]() {
    auto group = ctx.adjRibs.at(makePeerId(0))->getUpdateGroup();
    ASSERT_NE(group, nullptr);

    auto oldToken = group->getCancellationTokenForNewEgressPolicyReEvaluation();
    // Supersede: cancels oldToken, clears source.
    group->cancelEgressPolicyReEvaluation();
    // Create the replacement.
    auto newToken = group->getCancellationTokenForNewEgressPolicyReEvaluation();
    EXPECT_TRUE(oldToken.isCancellationRequested());
    EXPECT_FALSE(newToken.isCancellationRequested());
    EXPECT_TRUE(group->isEgressPolicyReEvaluationScheduled());

    if (!oldToken.isCancellationRequested()) {
      // A superseded task would reach here and reset; the guard keeps it out.
      group->resetEgressPolicyReEvaluationCancellationSource();
    }
    EXPECT_TRUE(group->isEgressPolicyReEvaluationScheduled());
    EXPECT_FALSE(newToken.isCancellationRequested());
  });
  tearDown(ctx);
}

} // namespace facebook::bgp
