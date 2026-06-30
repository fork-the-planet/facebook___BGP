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

} // namespace facebook::bgp
