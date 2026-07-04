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

#include <array>
#include <tuple>

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
    /*
     * Moved peers land at distinct bits; each carries its own state and sync
     * status -- peer 1 detached/out-of-sync, peer 2 in-sync.
     */
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

/*
 * When an update group completes its initial dump (with EoR), every member peer
 * must leave initial announcement. Otherwise setPendingEgressPolicyUpdate stays
 * a no-op (it ignores peers still in initial announcement) and egress policy
 * re-evaluation is silently suppressed for the group's peers.
 */
TEST_F(
    UpdateGroupPeerGroupAndPolicyReEvalTest,
    GroupInitialDump_ClearsInInitialAnnouncementForAllMembers) {
  auto ctx = setUp(2);
  auto& evb = ctx.peerMgr->getEventBase();

  // Before the group's initial dump, members are in initial announcement.
  evb.runInEventBaseThreadAndWait([&]() {
    for (auto& [peerId, adjRib] : ctx.adjRibs) {
      EXPECT_TRUE(adjRib->inInitialAnnouncement());
    }
  });

  sendInitialRibDump(ctx);

  // Completing the initial dump cleared it for every member.
  evb.runInEventBaseThreadAndWait([&]() {
    for (auto& [peerId, adjRib] : ctx.adjRibs) {
      EXPECT_FALSE(adjRib->inInitialAnnouncement());
    }
  });

  tearDown(ctx);
}

/*
 * End-to-end effect of clearing initial announcement after the dump: while a
 * member is still in initial announcement setPendingEgressPolicyUpdate is
 * suppressed; once the group's initial dump clears it, the same call is
 * honored. This is the behavior that unblocks egress policy re-evaluation.
 */
TEST_F(
    UpdateGroupPeerGroupAndPolicyReEvalTest,
    GroupInitialDump_EnablesPendingEgressPolicyUpdate) {
  auto ctx = setUp(2);
  auto& evb = ctx.peerMgr->getEventBase();
  auto peerId = makePeerId(0);

  // While in initial announcement, the pending egress flag is suppressed.
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(peerId);
    ASSERT_TRUE(adjRib->inInitialAnnouncement());
    adjRib->setPendingEgressPolicyUpdate(true);
    EXPECT_FALSE(adjRib->isEgressPolicyUpdateRequired());
  });

  sendInitialRibDump(ctx);

  // After the dump clears initial announcement, the same call is honored.
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRibs.at(peerId);
    ASSERT_FALSE(adjRib->inInitialAnnouncement());
    adjRib->setPendingEgressPolicyUpdate(true);
    EXPECT_TRUE(adjRib->isEgressPolicyUpdateRequired());
  });

  tearDown(ctx);
}

/*
 * processUpdateGroupsEgressPolicyReevaluation coverage, one behavior per test.
 * Each test stages policy changes (the async pipeline is disabled so the RPCs
 * only stage config), then drives the coroutine by hand once. The coroutine
 * rebuilds every peer's UpdateGroupKey, remaps members to their target key, and
 * reconciles each key into a single group -- keep an existing group at the key,
 * else rekey/split the most-in-sync contributor, then move the rest in --
 * re-evaluating a group only when its egress genuinely changed. Groups that
 * converge on one key (e.g. a partial-override group reset) are merged.
 */
class UpdateGroupsEgressReEvalTest : public UpdateGroupPolicyReEvalUTBase {};

/*
 * G1: a peer-group change plus a per-peer override on EVERY member makes the
 * whole group override, so it splits into a new group and the original empties
 * and is destroyed. A reset (unset overrides + reset the peer group to Policy0)
 * then restores the original {PG, Policy0, override=false} group.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_AllOverrideGroupDestroyedThenResetRestores) {
  constexpr int kN = 10;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb,
               [&]() {
                 return ctx.adjRibs.at(id)->getUpdateGroup()->getMemberCount();
               })
        .get();
  };
  auto baseline = keyOf(peer(0));

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // Mutate: peer-group change, then per-peer override on every member.
  disableAsyncEgressReEvalOnEvb(ctx);
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy1);
  for (int i = 0; i < kN; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // Original group emptied and was destroyed; all members in one override
  // group.
  EXPECT_FALSE(hasUpdateGroupOnEvb(ctx, baseline));
  EXPECT_TRUE(keyOf(peer(0)).peerOverride);
  EXPECT_EQ(memberCountOf(peer(0)), kN);

  // Reset: unset overrides + reset the peer group to Policy0.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kN; ++i) {
    unsetPeerEgressPolicyOnEvb(ctx, peer(i));
  }
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy0);
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // Back to the baseline group, all members restored.
  EXPECT_TRUE(keyOf(peer(0)) == baseline);
  EXPECT_TRUE(hasUpdateGroupOnEvb(ctx, baseline));
  EXPECT_EQ(memberCountOf(peer(0)), kN);

  // All members restored to Policy0's RIB-OUT.
  expectRibOutForPolicy(ctx, peer(0), kPolicy0);

  tearDown(ctx);
}

/*
 * G4: same net outcome as the all-override case, reached via a messier sequence
 * -- a per-peer override, a transient peer-group Policy2, override every
 * member, then peer-group Policy1. The group still ends fully overridden,
 * splits out, and the original is destroyed; reset restores it.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_OverrideViaMessySequenceDestroyedThenResetRestores) {
  constexpr int kN = 10;
  const std::string kPg = "PEERGROUP_4";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;
  const std::string kPolicy2 = kPNamePermitAll;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb,
               [&]() {
                 return ctx.adjRibs.at(id)->getUpdateGroup()->getMemberCount();
               })
        .get();
  };
  auto baseline = keyOf(peer(0));

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // Mutate: the messy sequence, net = all members overridden on Policy1.
  disableAsyncEgressReEvalOnEvb(ctx);
  updatePeerEgressPolicyOnEvb(ctx, peer(0), kPolicy1);
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy2);
  for (int i = 0; i < kN; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy1);
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  EXPECT_FALSE(hasUpdateGroupOnEvb(ctx, baseline));
  EXPECT_TRUE(keyOf(peer(0)).peerOverride);
  EXPECT_EQ(memberCountOf(peer(0)), kN);

  // Reset: unset overrides + reset the peer group to Policy0.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kN; ++i) {
    unsetPeerEgressPolicyOnEvb(ctx, peer(i));
  }
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy0);
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  EXPECT_TRUE(keyOf(peer(0)) == baseline);
  EXPECT_TRUE(hasUpdateGroupOnEvb(ctx, baseline));
  EXPECT_EQ(memberCountOf(peer(0)), kN);

  // All members restored to Policy0's RIB-OUT.
  expectRibOutForPolicy(ctx, peer(0), kPolicy0);

  tearDown(ctx);
}

/*
 * G3: a group that is never mutated is left completely alone -- same group
 * object, same key, same members -- even when a re-evaluation runs because a
 * different peer group changed. Two groups are set up; the "changed" one is
 * driven all-override (and destroyed) while the "untouched" one is verified
 * unaffected.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_UntouchedGroupUnaffectedByOtherGroupReEval) {
  constexpr int kN = 10;
  const std::string kChanged = "PEERGROUP_1";
  const std::string kUntouched = "PEERGROUP_3";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;

  // Changed peers -> [0,10), untouched peers -> [10,20).
  auto ctx = setUpGroups({{kChanged, kN}, {kUntouched, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto changed = [](int i) { return makePeerId(i); };
  auto untouched = [](int i) { return makePeerId(kN + i); };
  expectEventualStateOnEvb(ctx, changed(0), PeerUpdateState::JOINED_RUNNING);
  expectEventualStateOnEvb(ctx, untouched(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };

  auto untouchedBaseline = keyOf(untouched(0));
  auto untouchedGroupBefore = groupOf(untouched(0));

  std::vector<BgpPeerId> changedPeers;
  for (int i = 0; i < kN; ++i) {
    changedPeers.push_back(changed(i));
  }

  // Drive the changed group all-override so a re-evaluation actually runs.
  disableAsyncEgressReEvalOnEvb(ctx);
  updatePeerGroupEgressPolicyOnEvb(ctx, kChanged, kPolicy1);
  for (int i = 0; i < kN; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, changed(i), kPolicy1);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, changedPeers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // The untouched group is completely unaffected: same object, key, members.
  EXPECT_TRUE(keyOf(untouched(0)) == untouchedBaseline);
  EXPECT_TRUE(hasUpdateGroupOnEvb(ctx, untouchedBaseline));
  EXPECT_EQ(groupOf(untouched(0)), untouchedGroupBefore);
  EXPECT_EQ(memberCountOf(untouched(0)), kN);

  // The untouched group still advertises Policy0; the changed group
  // advertises Policy1.
  expectRibOutForPolicy(ctx, untouched(0), kPolicy0);
  expectRibOutForPolicy(ctx, changed(0), kPolicy1);

  tearDown(ctx);
}

/*
 * G2: a PARTIAL override -- a peer-group change plus per-peer overrides on only
 * SOME members -- splits the group into a non-override half (rekeyed in place)
 * and an override half (split into a new group). Resetting (unset the overrides
 * so the override half falls back to the peer-group egress, then reset the peer
 * group to Policy0) makes BOTH halves converge on the same
 * {PG, Policy0, override=false} key. Both halves see an egress-name change
 * (Policy1 -> Policy0), so both groups are re-evaluated; the reconcile keeps
 * one at the target key and merges the other into it, ending in a single group
 * with all members. (This is the "G2" case the earlier move-only reconcile
 * could not handle -- it overwrote the colliding map slot; see "problem case"
 * notes.)
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_PartialOverrideResetConvergesToSingleGroup) {
  constexpr int kN = 10;
  constexpr int kOverride = 5;
  const std::string kPg = "PEERGROUP_2";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  auto baseline = keyOf(peer(0));

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  /*
   * Reconcile #1: peer-group change to Policy1 + a per-peer override on the
   * first half. The non-override half is re-keyed in place; the override half
   * splits into a new group.
   */
  disableAsyncEgressReEvalOnEvb(ctx);
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy1);
  for (int i = 0; i < kOverride; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // One peer group is now backed by two update groups: an override half and a
  // non-override half.
  EXPECT_TRUE(keyOf(peer(0)).peerOverride);
  EXPECT_FALSE(keyOf(peer(kOverride)).peerOverride);
  EXPECT_EQ(memberCountOf(peer(0)), kOverride);
  EXPECT_EQ(memberCountOf(peer(kOverride)), kN - kOverride);

  /*
   * Reconcile #2: reset -- unset the overrides (override half falls back to the
   * peer-group egress) and reset the peer group to Policy0. Both halves now
   * want {PG, Policy0, override=false}.
   */
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kOverride; ++i) {
    unsetPeerEgressPolicyOnEvb(ctx, peer(i));
  }
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy0);
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // Correct end state: the two halves merge back into a single group with all
  // members, matching the original baseline key.
  EXPECT_TRUE(keyOf(peer(0)) == baseline);
  EXPECT_TRUE(hasUpdateGroupOnEvb(ctx, baseline));
  EXPECT_EQ(groupOf(peer(0)), groupOf(peer(kOverride)));
  EXPECT_EQ(memberCountOf(peer(0)), kN);
  // Both halves now advertise Policy0's RIB-OUT.
  expectRibOutForPolicy(ctx, peer(0), kPolicy0);
  expectRibOutForPolicy(ctx, peer(kOverride), kPolicy0);

  tearDown(ctx);
}

/*
 * Basic single-transition membership reshuffles between the base group states
 * (a group's key is {PeerGroup, egress, peerOverride}; same PeerGroup below):
 *   - S1: {Policy0, peerOverride=false}
 *   - S2: {Policy1, peerOverride=true}
 *   - S3: {Policy0, peerOverride=false} + {Policy1, peerOverride=true}
 *
 * Each test reaches its start state with one reconcile, then applies exactly
 * one transition and checks the resulting composition. Three of these are
 * merges -- S3 -> S2 (merge to override), S3 -> S1 (merge to non-override), and
 * the k-groups unset-all collapse -- and all merge correctly: the transition
 * changes the ex-override members' egress name, so their group is re-evaluated
 * and the reconcile merges its members into the group already at the target key
 * instead of overwriting it.
 */

/*
 * S3 -> S2 (merge to override): override the non-override half onto the
 * existing override policy so both halves converge on {PG, P1, true}. The
 * ex-non-override members flow through the bucketed move path into the existing
 * override group; the emptied non-override group is destroyed.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    BasicMembershipReshuffle_S3ToS2MergeToOverride) {
  constexpr int kN = 10;
  constexpr int kHalf = 5;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // Reach S3: override the first half onto P1 (peer-group egress stays P0).
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kHalf; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  ASSERT_TRUE(keyOf(peer(0)).peerOverride);
  ASSERT_FALSE(keyOf(peer(kHalf)).peerOverride);

  // Transition S3 -> S2: override the non-override half onto the same P1.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = kHalf; i < kN; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // One override group holds everyone; the old non-override group is gone.
  EXPECT_TRUE(keyOf(peer(0)).peerOverride);
  EXPECT_EQ(groupOf(peer(0)), groupOf(peer(kHalf)));
  EXPECT_EQ(memberCountOf(peer(0)), kN);

  // The merged override group advertises Policy1's RIB-OUT.
  expectRibOutForPolicy(ctx, peer(0), kPolicy1);
  expectRibOutForPolicy(ctx, peer(kHalf), kPolicy1);

  tearDown(ctx);
}

/*
 * S3 -> S3, egress converges but the override flag does not: move the
 * peer-group egress onto P1 while the override half is already on P1. The
 * non-override half becomes {PG, P1, false} and the override half stays
 * {PG, P1, true} -- same egress, different override flag -- so it is NOT a
 * merge; the group stays split. (Guards that only the flag+egress pair keys a
 * group.)
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    BasicMembershipReshuffle_SameEgressDistinctOverrideStaysSplit) {
  constexpr int kN = 10;
  constexpr int kHalf = 5;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // Reach S3: override the first half onto P1 (peer-group egress stays P0).
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kHalf; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  ASSERT_TRUE(keyOf(peer(0)).peerOverride);
  ASSERT_FALSE(keyOf(peer(kHalf)).peerOverride);

  // Transition: move the peer-group egress to P1 too. The non-override half
  // becomes {PG, P1, false}; the override half stays {PG, P1, true}.
  disableAsyncEgressReEvalOnEvb(ctx);
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy1);
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // Still two distinct groups (same egress, different override flag).
  EXPECT_NE(groupOf(peer(0)), groupOf(peer(kHalf)));
  EXPECT_TRUE(keyOf(peer(0)).peerOverride);
  EXPECT_FALSE(keyOf(peer(kHalf)).peerOverride);
  ASSERT_TRUE(keyOf(peer(0)).egressPolicyName.has_value());
  ASSERT_TRUE(keyOf(peer(kHalf)).egressPolicyName.has_value());
  EXPECT_EQ(*keyOf(peer(0)).egressPolicyName, kPolicy1);
  EXPECT_EQ(*keyOf(peer(kHalf)).egressPolicyName, kPolicy1);
  EXPECT_EQ(memberCountOf(peer(0)), kHalf);
  EXPECT_EQ(memberCountOf(peer(kHalf)), kN - kHalf);

  // Both groups advertise Policy1's RIB-OUT.
  expectRibOutForPolicy(ctx, peer(0), kPolicy1);
  expectRibOutForPolicy(ctx, peer(kHalf), kPolicy1);

  tearDown(ctx);
}

/*
 * S3 -> S1 (merge to non-override): unset the overrides with the peer-group
 * egress unchanged, so the ex-override members fall back from P1 to P0 and both
 * halves want {PG, P0, false}. That P1 -> P0 change re-evaluates the
 * ex-override group; the reconcile merges its members into the non-override
 * group already at {PG, P0, false} rather than overwriting it, ending in one
 * group.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    BasicMembershipReshuffle_S3ToS1MergeSameEgress) {
  constexpr int kN = 10;
  constexpr int kHalf = 5;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  auto baseline = keyOf(peer(0));

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // Reach S3: override the first half onto P1 (peer-group egress stays P0).
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kHalf; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  ASSERT_TRUE(keyOf(peer(0)).peerOverride);
  ASSERT_FALSE(keyOf(peer(kHalf)).peerOverride);

  // Transition S3 -> S1: unset the overrides; the peer-group egress stays P0.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kHalf; ++i) {
    unsetPeerEgressPolicyOnEvb(ctx, peer(i));
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // Correct end state: one merged non-override group with everyone.
  EXPECT_TRUE(keyOf(peer(0)) == baseline);
  EXPECT_EQ(groupOf(peer(0)), groupOf(peer(kHalf)));
  EXPECT_EQ(memberCountOf(peer(0)), kN);

  // Both halves now advertise Policy0's RIB-OUT.
  expectRibOutForPolicy(ctx, peer(0), kPolicy0);
  expectRibOutForPolicy(ctx, peer(kHalf), kPolicy0);

  tearDown(ctx);
}

/*
 * Unset-all collapse across THREE groups: a peer group split into a
 * non-override group + two distinct override groups (P1, P2). Unsetting every
 * override reverts P1 and P2 back to P0, so all three converge on {PG, P0,
 * false}. Both ex-override groups are re-evaluated (their egress name changed)
 * and the reconcile merges their members into the single non-override group at
 * the target key -- no overwrite; everyone ends in one group.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    BasicMembershipReshuffle_UnsetAllMergesGroups) {
  constexpr int kThird = 5;
  constexpr int kN = 3 * kThird;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;
  const std::string kPolicy2 = kPNamePermitAll;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);

  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  auto baseline = keyOf(peer(2 * kThird));

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // Reach 3 groups: override [0,kThird) onto P1 and [kThird,2*kThird) onto P2;
  // leave [2*kThird, kN) non-override on P0.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kThird; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  for (int i = kThird; i < 2 * kThird; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy2);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  ASSERT_NE(groupOf(peer(0)), groupOf(peer(kThird)));
  ASSERT_NE(groupOf(peer(0)), groupOf(peer(2 * kThird)));
  ASSERT_NE(groupOf(peer(kThird)), groupOf(peer(2 * kThird)));

  // Transition: unset every override -> all three groups want {PG, P0, false}.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < 2 * kThird; ++i) {
    unsetPeerEgressPolicyOnEvb(ctx, peer(i));
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // Correct end state: one merged non-override group with everyone.
  EXPECT_TRUE(keyOf(peer(0)) == baseline);
  EXPECT_EQ(groupOf(peer(0)), groupOf(peer(2 * kThird)));
  EXPECT_EQ(memberCountOf(peer(0)), kN);

  // All members now advertise Policy0's RIB-OUT.
  expectRibOutForPolicy(ctx, peer(0), kPolicy0);
  expectRibOutForPolicy(ctx, peer(2 * kThird), kPolicy0);

  tearDown(ctx);
}

/*
 * Rapid oscillation: repeatedly split off an override half and reset it,
 * cycling the override policy (P1, P2, P1). Every reset must merge back to a
 * single Policy0 group; the final RIB-OUT must be correct.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_RapidOscillation) {
  constexpr int kN = 10;
  constexpr int kHalf = 5;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;
  const std::string kPolicy2 = kPNamePermitAll;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);
  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  auto baseline = keyOf(peer(0));
  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  for (const auto& overridePolicy : {kPolicy1, kPolicy2, kPolicy1}) {
    // Split off the override half.
    disableAsyncEgressReEvalOnEvb(ctx);
    for (int i = 0; i < kHalf; ++i) {
      updatePeerEgressPolicyOnEvb(ctx, peer(i), overridePolicy);
    }
    markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
    runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
    EXPECT_TRUE(keyOf(peer(0)).peerOverride);
    EXPECT_NE(groupOf(peer(0)), groupOf(peer(kHalf)));

    // Reset: unset the override, merge back to one Policy0 group.
    disableAsyncEgressReEvalOnEvb(ctx);
    for (int i = 0; i < kHalf; ++i) {
      unsetPeerEgressPolicyOnEvb(ctx, peer(i));
    }
    markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
    runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
    EXPECT_TRUE(keyOf(peer(0)) == baseline);
    EXPECT_EQ(groupOf(peer(0)), groupOf(peer(kHalf)));
    EXPECT_EQ(memberCountOf(peer(0)), kN);
  }

  expectRibOutForPolicy(ctx, peer(0), kPolicy0);
  expectRibOutForPolicy(ctx, peer(kHalf), kPolicy0);

  tearDown(ctx);
}

/*
 * Three-way split, then a partial merge: a non-override group plus two override
 * groups (P1, P2). Unsetting only the P2 third merges it into the non-override
 * group while the P1 override group is left intact.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_ThreeWaySplitPartialMerge) {
  constexpr int kThird = 5;
  constexpr int kN = 3 * kThird;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;
  const std::string kPolicy2 = kPNamePermitAll;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);
  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // Reach three groups: [0,kThird) override P1, [kThird,2*kThird) override P2,
  // [2*kThird,kN) non-override P0.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kThird; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
  }
  for (int i = kThird; i < 2 * kThird; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy2);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  ASSERT_NE(groupOf(peer(0)), groupOf(peer(kThird)));
  ASSERT_NE(groupOf(peer(0)), groupOf(peer(2 * kThird)));
  ASSERT_NE(groupOf(peer(kThird)), groupOf(peer(2 * kThird)));
  expectRibOutForPolicy(ctx, peer(0), kPolicy1);
  expectRibOutForPolicy(ctx, peer(kThird), kPolicy2);
  expectRibOutForPolicy(ctx, peer(2 * kThird), kPolicy0);

  // Unset only the P2 third -> merges into the non-override P0 group.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = kThird; i < 2 * kThird; ++i) {
    unsetPeerEgressPolicyOnEvb(ctx, peer(i));
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // P2 third merged into the non-override group; P1 override group unchanged.
  EXPECT_EQ(groupOf(peer(kThird)), groupOf(peer(2 * kThird)));
  EXPECT_EQ(memberCountOf(peer(2 * kThird)), 2 * kThird);
  EXPECT_TRUE(keyOf(peer(0)).peerOverride);
  EXPECT_EQ(memberCountOf(peer(0)), kThird);
  EXPECT_NE(groupOf(peer(0)), groupOf(peer(2 * kThird)));
  expectRibOutForPolicy(ctx, peer(2 * kThird), kPolicy0);
  expectRibOutForPolicy(ctx, peer(kThird), kPolicy0);
  expectRibOutForPolicy(ctx, peer(0), kPolicy1);

  tearDown(ctx);
}

/*
 * Cross-policy churn: move the peer-group egress across P0 -> P1 -> P2 while a
 * subset overrides, exercising in-place rekey, split,
 * same-egress/different-flag (no merge), and a final unset-driven merge.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_CrossPolicyChurn) {
  constexpr int kN = 10;
  constexpr int kHalf = 5;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;
  const std::string kPolicy2 = kPNamePermitAll;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);
  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  // A: peer-group egress P0 -> P1 (all non-override rekey in place).
  disableAsyncEgressReEvalOnEvb(ctx);
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy1);
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  EXPECT_FALSE(keyOf(peer(0)).peerOverride);
  EXPECT_EQ(memberCountOf(peer(0)), kN);
  expectRibOutForPolicy(ctx, peer(0), kPolicy1);

  // B: override the first half to P2 -> split.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kHalf; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy2);
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  EXPECT_NE(groupOf(peer(0)), groupOf(peer(kHalf)));
  EXPECT_TRUE(keyOf(peer(0)).peerOverride);
  expectRibOutForPolicy(ctx, peer(0), kPolicy2);
  expectRibOutForPolicy(ctx, peer(kHalf), kPolicy1);

  // C: peer-group egress P1 -> P2: non-override half becomes {P2,false}; the
  // override half is {P2,true} -- same egress, different flag -> stays split.
  disableAsyncEgressReEvalOnEvb(ctx);
  updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kPolicy2);
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  EXPECT_NE(groupOf(peer(0)), groupOf(peer(kHalf)));
  ASSERT_TRUE(keyOf(peer(kHalf)).egressPolicyName.has_value());
  EXPECT_EQ(*keyOf(peer(kHalf)).egressPolicyName, kPolicy2);
  expectRibOutForPolicy(ctx, peer(0), kPolicy2);
  expectRibOutForPolicy(ctx, peer(kHalf), kPolicy2);

  // D: unset overrides -> merge to one {P2,false} group.
  disableAsyncEgressReEvalOnEvb(ctx);
  for (int i = 0; i < kHalf; ++i) {
    unsetPeerEgressPolicyOnEvb(ctx, peer(i));
  }
  markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
  EXPECT_EQ(groupOf(peer(0)), groupOf(peer(kHalf)));
  EXPECT_EQ(memberCountOf(peer(0)), kN);
  EXPECT_FALSE(keyOf(peer(0)).peerOverride);
  expectRibOutForPolicy(ctx, peer(0), kPolicy2);

  tearDown(ctx);
}

/*
 * Split-and-remerge: drive S1 -> S3 -> S1 repeatedly with the same override
 * policy, stressing the rekey-vs-merge survivor choice across rounds. Ends as
 * one Policy0 group with correct RIB-OUT.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_SplitAndRemerge) {
  constexpr int kN = 10;
  constexpr int kHalf = 5;
  const std::string kPg = "PEERGROUP_1";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;

  auto ctx = setUpGroups({{kPg, kN}}, true, kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);
  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  auto baseline = keyOf(peer(0));
  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  for (int round = 0; round < 3; ++round) {
    // Split: override the first half to P1.
    disableAsyncEgressReEvalOnEvb(ctx);
    for (int i = 0; i < kHalf; ++i) {
      updatePeerEgressPolicyOnEvb(ctx, peer(i), kPolicy1);
    }
    markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
    runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
    EXPECT_NE(groupOf(peer(0)), groupOf(peer(kHalf)));

    // Remerge: unset -> back to one Policy0 group.
    disableAsyncEgressReEvalOnEvb(ctx);
    for (int i = 0; i < kHalf; ++i) {
      unsetPeerEgressPolicyOnEvb(ctx, peer(i));
    }
    markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
    runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
    EXPECT_EQ(groupOf(peer(0)), groupOf(peer(kHalf)));
    EXPECT_EQ(memberCountOf(peer(0)), kN);
    EXPECT_TRUE(keyOf(peer(0)) == baseline);
  }

  expectRibOutForPolicy(ctx, peer(0), kPolicy0);
  expectRibOutForPolicy(ctx, peer(kHalf), kPolicy0);

  tearDown(ctx);
}

/*
 * All group shapes reconciled in one pass: four peer groups driven into
 * distinct shapes -- fully overridden (A), partially overridden (B),
 * peer-group rekey (C), and untouched (D) -- then a single reconcile must land
 * each in its correct state with correct RIB-OUT.
 */
TEST_F(
    UpdateGroupsEgressReEvalTest,
    CompositeMembershipReshuffle_AllGroupShapesInOneReeval) {
  constexpr int kN = 10;
  constexpr int kHalf = 5;
  const std::string kAllOvr = "PEERGROUP_A";
  const std::string kPartial = "PEERGROUP_B";
  const std::string kRekey = "PEERGROUP_C";
  const std::string kUntouched = "PEERGROUP_D";
  const std::string kPolicy0 = kPNameMatchNoAdvtDeny;
  const std::string kPolicy1 = kPNameMatchModifyAppend;
  const std::string kPolicy2 = kPNamePermitAll;

  auto ctx = setUpGroups(
      {{kAllOvr, kN}, {kPartial, kN}, {kRekey, kN}, {kUntouched, kN}},
      true,
      kPolicy0);
  sendInitialRibDump(ctx);
  auto peer = [](int i) { return makePeerId(i); };
  // Global index bases: A=[0,10), B=[10,20), C=[20,30), D=[30,40).
  auto a = [&](int i) { return peer(i); };
  auto b = [&](int i) { return peer(kN + i); };
  auto c = [&](int i) { return peer(2 * kN + i); };
  auto d = [&](int i) { return peer(3 * kN + i); };
  expectEventualStateOnEvb(ctx, a(0), PeerUpdateState::JOINED_RUNNING);
  expectEventualStateOnEvb(ctx, d(0), PeerUpdateState::JOINED_RUNNING);
  auto& evb = ctx.peerMgr->getEventBase();
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return groupOf(id)->getMemberCount();
  };
  auto dBaseline = keyOf(d(0));
  std::vector<BgpPeerId> allPeers;
  for (int i = 0; i < 4 * kN; ++i) {
    allPeers.push_back(peer(i));
  }

  // Stage all shapes, then run ONE reconcile.
  disableAsyncEgressReEvalOnEvb(ctx);
  // A: fully overridden to P1.
  for (int i = 0; i < kN; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, a(i), kPolicy1);
  }
  // B: first half overridden to P1 (rest stays P0).
  for (int i = 0; i < kHalf; ++i) {
    updatePeerEgressPolicyOnEvb(ctx, b(i), kPolicy1);
  }
  // C: peer-group egress rekey P0 -> P2.
  updatePeerGroupEgressPolicyOnEvb(ctx, kRekey, kPolicy2);
  // D: untouched.
  markEgressPolicyUpdateRequiredOnEvb(ctx, allPeers);
  runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);

  // A: one override group, all members, P1.
  EXPECT_TRUE(keyOf(a(0)).peerOverride);
  EXPECT_EQ(memberCountOf(a(0)), kN);
  expectRibOutForPolicy(ctx, a(0), kPolicy1);
  // B: split -- override half P1, non-override half P0.
  EXPECT_NE(groupOf(b(0)), groupOf(b(kHalf)));
  EXPECT_EQ(memberCountOf(b(0)), kHalf);
  EXPECT_EQ(memberCountOf(b(kHalf)), kN - kHalf);
  expectRibOutForPolicy(ctx, b(0), kPolicy1);
  expectRibOutForPolicy(ctx, b(kHalf), kPolicy0);
  // C: rekeyed in place to P2, all members, non-override.
  EXPECT_FALSE(keyOf(c(0)).peerOverride);
  EXPECT_EQ(memberCountOf(c(0)), kN);
  expectRibOutForPolicy(ctx, c(0), kPolicy2);
  // D: untouched, still P0.
  EXPECT_TRUE(keyOf(d(0)) == dBaseline);
  EXPECT_EQ(memberCountOf(d(0)), kN);
  expectRibOutForPolicy(ctx, d(0), kPolicy0);

  tearDown(ctx);
}

/*
 * Drain/undrain sequence matrix. The six bundle/node drain-undrain orderings
 * from the "Stateful Policy Resolution for BGP++ Drain Workflows" design, each
 * run under all eight ways processUpdateGroupsEgressPolicyReevaluation can pick
 * up the intermediate config changes.
 *
 * The group starts under a filtering (non-permit-all) egress policy, and the
 * initial dump is verified to apply that policy (not a permit-all pass-through)
 * before the sequence runs. The matrix runs at 1000 peers, with the bundle
 * (per-peer) operations acting on the first 25% of them.
 *
 * Each sequence is four operations A B C D with three optional pick-up points
 * between them (after A, B, C); the coroutine ALWAYS runs once more after D.
 * That is 2^3 = 8 pick-up configurations (bit i set == re-evaluate after op i),
 * so 6 sequences x 8 configs = 48 cases. Because the re-evaluation reconciles
 * group membership from the full current config on every run, all eight configs
 * of a sequence converge to the same state -- and all six sequences end fully
 * undrained: one group holding every peer on the undrain policy, with the
 * matching RIB-OUT.
 */
enum class DrainOp { BundleDrain, BundleUndrain, NodeDrain, NodeUndrain };

inline constexpr std::array<std::array<DrainOp, 4>, 6> kDrainSequences = {{
    // Seq 1: Bundle Drain -> Bundle Undrain -> Node Drain -> Node Undrain
    {DrainOp::BundleDrain,
     DrainOp::BundleUndrain,
     DrainOp::NodeDrain,
     DrainOp::NodeUndrain},
    // Seq 2: Bundle Drain -> Node Drain -> Bundle Undrain -> Node Undrain
    {DrainOp::BundleDrain,
     DrainOp::NodeDrain,
     DrainOp::BundleUndrain,
     DrainOp::NodeUndrain},
    // Seq 3: Node Drain -> Node Undrain -> Bundle Drain -> Bundle Undrain
    {DrainOp::NodeDrain,
     DrainOp::NodeUndrain,
     DrainOp::BundleDrain,
     DrainOp::BundleUndrain},
    // Seq 4: Node Drain -> Bundle Drain -> Bundle Undrain -> Node Undrain
    {DrainOp::NodeDrain,
     DrainOp::BundleDrain,
     DrainOp::BundleUndrain,
     DrainOp::NodeUndrain},
    // Seq 5: Bundle Drain -> Node Drain -> Node Undrain -> Bundle Undrain
    {DrainOp::BundleDrain,
     DrainOp::NodeDrain,
     DrainOp::NodeUndrain,
     DrainOp::BundleUndrain},
    // Seq 6: Node Drain -> Bundle Drain -> Node Undrain -> Bundle Undrain
    {DrainOp::NodeDrain,
     DrainOp::BundleDrain,
     DrainOp::NodeUndrain,
     DrainOp::BundleUndrain},
}};

class DrainSequencePickupTest
    : public UpdateGroupPolicyReEvalUTBase,
      public ::testing::WithParamInterface<std::tuple<int, int>> {};

TEST_P(DrainSequencePickupTest, ConvergesRegardlessOfPickupTiming) {
  const int seqIdx = std::get<0>(GetParam());
  const int pickupCfg = std::get<1>(GetParam());
  const std::string kDrain = kPNameMatchNoAdvtDeny;
  const std::string kUndrain = kPNamePermitAll;
  const std::string kPg = "PEERGROUP_A";
  constexpr int kN = 1000;
  // Bundle (per-peer) drain/undrain acts on the first 25% of the group's peers.
  const int kBundleCount = kN * 25 / 100;

  // Start under a filtering (non-permit-all) egress policy so the initial dump
  // exercises real policy application, not a permit-all pass-through.
  auto ctx = setUpGroups({{kPg, kN}}, /*initialDumpCompleted=*/true, kDrain);
  auto peer = [](int i) { return makePeerId(i); };
  sendInitialRibDump(ctx);
  expectEventualStateOnEvb(ctx, peer(0), PeerUpdateState::JOINED_RUNNING);
  // Initial dump under the drain policy: 50 routes advertised, 50 denied.
  expectRibOutForPolicy(ctx, peer(0), kDrain);

  std::vector<BgpPeerId> peers;
  for (int i = 0; i < kN; ++i) {
    peers.push_back(peer(i));
  }

  auto& evb = ctx.peerMgr->getEventBase();
  auto groupOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroup(); })
        .get();
  };
  auto keyOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb, [&]() { return ctx.adjRibs.at(id)->getUpdateGroupKey(); })
        .get();
  };
  auto memberCountOf = [&](const BgpPeerId& id) {
    return folly::via(
               &evb,
               [&]() {
                 return ctx.adjRibs.at(id)->getUpdateGroup()->getMemberCount();
               })
        .get();
  };

  auto applyOp = [&](DrainOp op) {
    switch (op) {
      case DrainOp::BundleDrain:
        for (int i = 0; i < kBundleCount; ++i) {
          updatePeerEgressPolicyOnEvb(ctx, peer(i), kDrain);
        }
        break;
      case DrainOp::BundleUndrain:
        for (int i = 0; i < kBundleCount; ++i) {
          unsetPeerEgressPolicyOnEvb(ctx, peer(i));
        }
        break;
      case DrainOp::NodeDrain:
        updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kDrain);
        break;
      case DrainOp::NodeUndrain:
        updatePeerGroupEgressPolicyOnEvb(ctx, kPg, kUndrain);
        break;
    }
  };

  const auto& seq = kDrainSequences[seqIdx];
  for (size_t i = 0; i < seq.size(); ++i) {
    // Re-arm the async re-eval guard before each API call so only our manual
    // pickups drive re-evaluation; a manual pickup clears the guard on exit.
    disableAsyncEgressReEvalOnEvb(ctx);
    applyOp(seq[i]);
    // Pick up after earlier ops per the config bitmask; always after the last.
    if (i == seq.size() - 1 || (pickupCfg & (1 << i)) != 0) {
      markEgressPolicyUpdateRequiredOnEvb(ctx, peers);
      runProcessUpdateGroupsEgressPolicyReevaluationOnEvb(ctx);
    }
  }

  // Every sequence, under every pickup config, ends fully undrained: one group
  // holding all peers on the undrain policy, no per-peer override, matching
  // RIB-OUT.
  for (const auto& p : peers) {
    EXPECT_EQ(groupOf(peer(0)), groupOf(p));
    expectRibOutForPolicy(ctx, p, kUndrain);
  }
  EXPECT_EQ(memberCountOf(peer(0)), static_cast<size_t>(kN));
  EXPECT_FALSE(keyOf(peer(0)).peerOverride);
  ASSERT_TRUE(keyOf(peer(0)).egressPolicyName.has_value());
  EXPECT_EQ(*keyOf(peer(0)).egressPolicyName, kUndrain);

  tearDown(ctx);
}

INSTANTIATE_TEST_SUITE_P(
    DrainWorkflows,
    DrainSequencePickupTest,
    ::testing::Combine(::testing::Range(0, 6), ::testing::Range(0, 8)),
    [](const ::testing::TestParamInfo<std::tuple<int, int>>& info) {
      return "Seq" + std::to_string(std::get<0>(info.param) + 1) + "Cfg" +
          std::to_string(std::get<1>(info.param));
    });

} // namespace facebook::bgp
