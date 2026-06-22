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

#define PeerManager_TEST_FRIENDS                                   \
  friend class UpdateGroupDynamicPolicyReEvaluationTest;           \
  FRIEND_TEST(                                                     \
      UpdateGroupDynamicPolicyReEvaluationTest,                    \
      GetPolicyReEvalPendingGroups_EmptyWhenNoPendingPeers);       \
  FRIEND_TEST(                                                     \
      UpdateGroupDynamicPolicyReEvaluationTest,                    \
      GetPolicyReEvalPendingGroups_CollectsPendingPeerGroups);     \
  FRIEND_TEST(                                                     \
      UpdateGroupDynamicPolicyReEvaluationTest,                    \
      GetPolicyReEvalPendingGroups_DeduplicatesSharedGroup);       \
  FRIEND_TEST(                                                     \
      UpdateGroupDynamicPolicyReEvaluationTest,                    \
      GetPolicyReEvalPendingGroups_SkipsInitialAnnouncementPeers); \
  FRIEND_TEST(                                                     \
      UpdateGroupDynamicPolicyReEvaluationTest,                    \
      ProcessGroupEgressPolicyReEvaluation_HandlesAllPeerStates);  \
  FRIEND_TEST(                                                     \
      UpdateGroupDynamicPolicyReEvaluationTest,                    \
      ProcessRibDumpReq_ReschedulesTimerForDetachedPeer);

#define AdjRib_TEST_FRIENDS                              \
  friend class UpdateGroupDynamicPolicyReEvaluationTest; \
  friend class SinglePeerPolicyReEvaluation;             \
  FRIEND_TEST(                                           \
      UpdateGroupDynamicPolicyReEvaluationTest,          \
      ProcessRibDumpReq_ReschedulesTimerForDetachedPeer);

#include <gtest/gtest.h>

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ChangeTracker.h"
#include "neteng/fboss/bgp/cpp/changeTracker/TrackableObject.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"
#include "neteng/fboss/bgp/cpp/tests/PeerManagerTestUtils.h"

namespace facebook::bgp {

class UpdateGroupDynamicPolicyReEvaluationTest : public PeerManagerTestFixture {
 protected:
  struct TestContext {
    std::shared_ptr<PeerManager> peerMgr;
    std::shared_ptr<SessionManager> sessionMgr;
    std::shared_ptr<AdjRib> adjRib1;
    std::shared_ptr<AdjRib> adjRib2;
    std::shared_ptr<AdjRib> adjRib3;
    std::thread peerMgrThread;
    std::thread sessionMgrThread;
  };

  std::shared_ptr<AdjRib> setupAdjRib(
      folly::EventBase& evb,
      std::shared_ptr<ChangeTracker<ShadowRibEntry>> /* changeListTracker */,
      const nettools::bgplib::BgpPeerId& peerId,
      const AsNum& remoteAs,
      std::shared_ptr<folly::coro::Baton>& sessionTerminateBaton,
      std::shared_ptr<const Config> config,
      const std::string& description,
      const std::string& peerGroupName) {
    auto adjRibOutGroup =
        std::make_shared<AdjRibOutGroup>(evb, "PeerManagerTest");
    auto adjRib = std::make_shared<AdjRib>(
        peerId,
        PeeringParams(
            peerId.peerAddr,
            std::nullopt,
            kAsn1,
            kAsn1,
            remoteAs,
            kLocalAddr1.asV4(),
            kLocalAddr1.asV4(),
            std::chrono::seconds(kDefaultHoldTime),
            std::chrono::seconds(kGrRestartTime),
            nettools::bgplib::constants::kBgpPort,
            folly::AsyncSocket::anyAddress(),
            TBgpSessionConnectMode::PASSIVE_ACTIVE,
            kV4Nexthop1.asV4(),
            kV6Nexthop1.asV6(),
            RrClientConfigured(false),
            NextHopSelfConfigured{false},
            AfiIpv4Configured{true},
            AfiIpv6Configured{true},
            ConfedPeerConfigured{false},
            RemovePrivateAsConfigured{false},
            std::nullopt,
            std::nullopt,
            AdvertiseLinkBandwidth::DISABLE,
            ReceiveLinkBandwidth::ACCEPT,
            std::nullopt,
            ValidateRemoteAs{true},
            std::nullopt,
            std::nullopt,
            false,
            EnableStatefulHa{false},
            std::nullopt,
            V4OverV6Nexthop{false}),
        evb,
        ribInQ_,
        observerQ_,
        sessionTerminateBaton,
        nullptr,
        isSafeModeOn_,
        std::nullopt,
        std::nullopt,
        adjRibOutGroup,
        std::nullopt,
        std::make_shared<ConfigManager>(config));
    adjRib->peeringParams_.description = description;
    adjRib->peeringParams_.peerGroupName = peerGroupName;
    return adjRib;
  }

  TestContext setUp(bool enableUpdateGroup = true) {
    auto config = getConfig(
        true, // includeStaticPeer
        true, // includeDynamicShivPeer
        false, // includeDynamicMonitorPeer
        false, // includeDynamicVipInjectorPeer
        false, // enableStatefulHa
        true, // enableVipServer
        kDefaultEorTimeS, // eorTimeS
        false, // enableSubscriberLimit
        false, // enableSwitchLimit
        false, // applyGoldenPrefixPolicy
        {}, // bgpFeatures
        false, // enableDynamicPolicyEvaluation
        enableUpdateGroup);

    auto configManager = std::make_shared<ConfigManager>(config);
    auto peerMgr = std::make_shared<PeerManager>(
        configManager, nullptr, ribInQ_, ribOutQ_, nbrRouteChangeQ_);

    auto globalConfig = config->getBgpGlobalConfig();
    auto sessionMgr = std::make_shared<SessionManager>(*globalConfig, false);
    peerMgr->setSessionManager(sessionMgr);

    auto& evb = peerMgr->getEventBase();

    auto adjRib1 = setupAdjRib(
        evb,
        peerMgr->getChangeListTracker(),
        kPeerId1,
        AsNum(kAsn1),
        sessionTerminateBaton_,
        config,
        "adjRib1",
        "PEERGROUP_RSW_FSW_V4");
    auto group1 = std::make_shared<AdjRibOutGroup>(evb, "Group1");
    adjRib1->adjRibOutGroup_ = group1;
    adjRib1->resetInInitialAnnouncement();
    group1->registerPeer(adjRib1);

    auto adjRib2 = setupAdjRib(
        evb,
        peerMgr->getChangeListTracker(),
        kPeerId2,
        AsNum(kAsn1),
        sessionTerminateBaton_,
        config,
        "adjRib2",
        "PEERGROUP_FSW_RSW_V4");
    auto group2 = std::make_shared<AdjRibOutGroup>(evb, "Group2");
    adjRib2->adjRibOutGroup_ = group2;
    adjRib2->resetInInitialAnnouncement();
    group2->registerPeer(adjRib2);

    auto adjRib3 = setupAdjRib(
        evb,
        peerMgr->getChangeListTracker(),
        kPeerId3,
        AsNum(kAsn1),
        sessionTerminateBaton_,
        config,
        "adjRib3",
        "PEERGROUP_RSW_FSW_V4");
    auto group3 = std::make_shared<AdjRibOutGroup>(evb, "Group3");
    adjRib3->adjRibOutGroup_ = group3;
    adjRib3->resetInInitialAnnouncement();
    group3->registerPeer(adjRib3);

    peerMgr->adjRibs_[kPeerId1] = adjRib1;
    peerMgr->adjRibs_[kPeerId2] = adjRib2;
    peerMgr->adjRibs_[kPeerId3] = adjRib3;

    auto peerMgrThread = peerMgr->runInThread();
    auto sessionMgrThread = sessionMgr->runInThread();

    return {
        peerMgr,
        sessionMgr,
        adjRib1,
        adjRib2,
        adjRib3,
        std::move(peerMgrThread),
        std::move(sessionMgrThread)};
  }

  void tearDown(TestContext& ctx) {
    ctx.peerMgr->stop();
    ctx.sessionMgr->stop();
    ctx.peerMgrThread.join();
    ctx.sessionMgrThread.join();
  }
};

TEST_F(
    UpdateGroupDynamicPolicyReEvaluationTest,
    GetPolicyReEvalPendingGroups_EmptyWhenNoPendingPeers) {
  auto ctx = setUp(true /* enableUpdateGroup */);
  auto& evb = ctx.peerMgr->getEventBase();

  evb.runInEventBaseThreadAndWait([&]() {
    auto groups = ctx.peerMgr->getPolicyReEvalPendingGroups();
    EXPECT_EQ(groups.size(), 0);
  });

  tearDown(ctx);
}

TEST_F(
    UpdateGroupDynamicPolicyReEvaluationTest,
    GetPolicyReEvalPendingGroups_CollectsPendingPeerGroups) {
  auto ctx = setUp(true /* enableUpdateGroup */);
  auto& evb = ctx.peerMgr->getEventBase();

  evb.runInEventBaseThreadAndWait([&]() {
    ctx.adjRib1->setPendingEgressPolicyUpdate(true);
    ctx.adjRib2->setPendingEgressPolicyUpdate(true);
    auto groups = ctx.peerMgr->getPolicyReEvalPendingGroups();
    EXPECT_EQ(groups.size(), 2);
  });

  tearDown(ctx);
}

TEST_F(
    UpdateGroupDynamicPolicyReEvaluationTest,
    GetPolicyReEvalPendingGroups_DeduplicatesSharedGroup) {
  auto ctx = setUp(true /* enableUpdateGroup */);
  auto& evb = ctx.peerMgr->getEventBase();

  // Move adjRib2 into adjRib1's group
  auto group2 = ctx.adjRib2->getUpdateGroup();
  group2->unregisterPeer(ctx.adjRib2);
  auto group1 = ctx.adjRib1->getUpdateGroup();
  group1->registerPeer(ctx.adjRib2);
  ctx.adjRib2->setUpdateGroup(group1);

  evb.runInEventBaseThreadAndWait([&]() {
    ctx.adjRib1->setPendingEgressPolicyUpdate(true);
    ctx.adjRib2->setPendingEgressPolicyUpdate(true);
    auto groups = ctx.peerMgr->getPolicyReEvalPendingGroups();
    EXPECT_EQ(groups.size(), 1);
    EXPECT_TRUE(groups.count(group1));
  });

  tearDown(ctx);
}

TEST_F(
    UpdateGroupDynamicPolicyReEvaluationTest,
    GetPolicyReEvalPendingGroups_SkipsInitialAnnouncementPeers) {
  auto ctx = setUp(true /* enableUpdateGroup */);
  auto& evb = ctx.peerMgr->getEventBase();

  ctx.adjRib1->setInInitialAnnouncement();

  evb.runInEventBaseThreadAndWait([&]() {
    ctx.adjRib1->setPendingEgressPolicyUpdate(
        true); // no-op: inInitialAnnouncement
    ctx.adjRib2->setPendingEgressPolicyUpdate(true);
    EXPECT_FALSE(ctx.adjRib1->isEgressPolicyUpdateRequired());
    auto groups = ctx.peerMgr->getPolicyReEvalPendingGroups();
    EXPECT_EQ(groups.size(), 1);
    EXPECT_TRUE(groups.count(ctx.adjRib2->getUpdateGroup()));
  });

  tearDown(ctx);
}

/**
 * processGroupEgressPolicyReEvaluation handles three peer states:
 *   IN_SYNC: clears pendingEgressPolicyUpdate immediately
 *   DETACHED (no rib dump scheduled): re-walks the ShadowRib via
 *     processRibDumpReq, which clears pending after
 *   DETACHED + rib dump scheduled: defers (pending flag stays set)
 */
TEST_F(
    UpdateGroupDynamicPolicyReEvaluationTest,
    ProcessGroupEgressPolicyReEvaluation_HandlesAllPeerStates) {
  auto ctx = setUp(true /* enableUpdateGroup */);
  auto& evb = ctx.peerMgr->getEventBase();

  // Put all three adjRibs into the same group
  auto group = ctx.adjRib1->getUpdateGroup();
  ctx.adjRib2->getUpdateGroup()->unregisterPeer(ctx.adjRib2);
  group->registerPeer(ctx.adjRib2);
  ctx.adjRib2->setUpdateGroup(group);
  ctx.adjRib3->getUpdateGroup()->unregisterPeer(ctx.adjRib3);
  group->registerPeer(ctx.adjRib3);
  ctx.adjRib3->setUpdateGroup(group);

  // adjRib1: IN_SYNC (default)
  // adjRib2: DETACHED_RUNNING without a rib dump scheduled
  // adjRib3: DETACHED_INIT_DUMP with a rib dump scheduled
  ctx.adjRib2->setPeerState(PeerUpdateState::DETACHED_RUNNING);
  ctx.adjRib3->setPeerState(PeerUpdateState::DETACHED_INIT_DUMP);
  /*
   * adjRib3 has a rib dump in flight: hold it via testOnlyDeferInitDump (the
   * scheduled dump self-reschedules instead of completing, keeping its
   * cancellation source in place) and schedule it so isRibDumpScheduled() stays
   * true when re-eval runs.
   */
  ctx.adjRib3->testOnlyDeferInitDump = true;
  evb.runInEventBaseThreadAndWait(
      [&]() { ctx.peerMgr->scheduleRibDumpForAdjRib(ctx.adjRib3); });

  evb.runInEventBaseThreadAndWait([&]() {
    ctx.adjRib1->setPendingEgressPolicyUpdate(true);
    ctx.adjRib2->setPendingEgressPolicyUpdate(true);
    ctx.adjRib3->setPendingEgressPolicyUpdate(true);
  });

  folly::coro::blockingWait(
      folly::coro::co_withExecutor(
          &evb, ctx.peerMgr->processGroupEgressPolicyReEvaluation(group)));

  // All assertions run on the evb thread to avoid racing with the async
  // processRibDumpReq scheduled by processGroupEgressPolicyReEvaluation.
  WITH_RETRIES({
    evb.runInEventBaseThreadAndWait([&]() {
      // adjRib1 (IN_SYNC): flag cleared immediately
      EXPECT_EVENTUALLY_FALSE(ctx.adjRib1->isEgressPolicyUpdateRequired());

      // adjRib2 (DETACHED_RUNNING): pending flag cleared after async rib dump
      EXPECT_EVENTUALLY_FALSE(ctx.adjRib2->isEgressPolicyUpdateRequired());

      // adjRib3 (DETACHED_INIT_DUMP + rib dump scheduled): deferred
      EXPECT_EVENTUALLY_TRUE(ctx.adjRib3->isEgressPolicyUpdateRequired());
    });
  });

  tearDown(ctx);
}

/**
 * Get a peer into DETACHED_READY_TO_JOIN via the DFP path, then verify
 * that processRibDumpReq reschedules the changeListConsumeTimer_ via
 * activateChangeListConsumer -> scheduleSendBgpUpdates -> sendBgpUpdates
 * -> reschedulePackingTimers.
 */
TEST_F(
    UpdateGroupDynamicPolicyReEvaluationTest,
    ProcessRibDumpReq_ReschedulesTimerForDetachedPeer) {
  auto ctx = setUp(true /* enableUpdateGroup */);
  auto& evb = ctx.peerMgr->getEventBase();

  // Equip adjRib1 with queues, backpressure, and AFI negotiation so the
  // full send pipeline (processRibMessage -> sendBgpUpdates) works.
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRib1;
    adjRib->adjRibOutQueue_ = std::make_shared<AdjRib::AdjRibOutQueueT>();
    adjRib->boundedAdjRibOutQueue_ =
        std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(10, 8, 2);
    adjRib->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
    adjRib->enableEgressQueueBackpressure(true);
    adjRib->isAfiIpv4Negotiated_ = true;
  });

  // Phase 1: Populate the ShadowRib with 5 prefixes via
  // handleShadowRibEntryAnnouncement so processRibDumpReq has entries to walk.
  evb.runInEventBaseThreadAndWait([&]() {
    auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
    attrs->publish();
    // Use kPeerAddr2 as originator — canAnnounce() rejects routes from the
    // same peer (kPeerAddr1) that the AdjRib belongs to.
    TinyPeerInfo peer(
        kPeerAddr2, kAsn1, kPeerRouterId2, BgpSessionType::EBGP, false);

    RibOutAnnouncement announcement;
    announcement.initialDump = true;
    for (int i = 1; i <= 5; ++i) {
      announcement.entries.emplace_back(
          folly::CIDRNetwork{folly::IPAddress(fmt::format("10.0.{}.0", i)), 24},
          kDefaultPathID,
          peer,
          attrs);
      announcement.entries.back().ribVersion = i;
    }
    ctx.peerMgr->handleShadowRibEntryAnnouncement(announcement);
  });

  // Phase 2: Register the group consumer on adjRib1's group.
  // Use PeerManager's bitmaps so publishChange stamps consumer bits correctly.
  auto& addPathBitmap = ctx.peerMgr->addPathConsumerBitmap_;
  auto& nonAddPathBitmap = ctx.peerMgr->nonAddPathConsumerBitmap_;
  std::shared_ptr<AdjRibOutGroupConsumer> groupConsumer;
  evb.runInEventBaseThreadAndWait([&]() {
    auto group = ctx.adjRib1->getUpdateGroup();
    groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
        ctx.peerMgr->changeListTracker_,
        group,
        "test_group_consumer",
        evb,
        addPathBitmap,
        nonAddPathBitmap);
    groupConsumer->registerWithTracker();
    groupConsumer->setBitmap();
    group->setChangeListConsumer(groupConsumer);
    group->setChangeListTracker(
        ctx.peerMgr->changeListTracker_, addPathBitmap, nonAddPathBitmap);
  });

  // Phase 3: Publish one more prefix to ShadowRib. This item lands on
  // the changelist after the group consumer is registered, so the
  // per-peer consumer will have unconsumed items (isReady() == false).
  evb.runInEventBaseThreadAndWait([&]() {
    auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
    attrs->publish();
    TinyPeerInfo peer(
        kPeerAddr2, kAsn1, kPeerRouterId2, BgpSessionType::EBGP, false);

    RibOutAnnouncement announcement;
    for (int i = 0; i < 1; ++i) {
      announcement.entries.emplace_back(
          folly::CIDRNetwork{folly::IPAddress("10.1.0.0"), 24},
          kDefaultPathID,
          peer,
          attrs);
    }
    ctx.peerMgr->handleShadowRibEntryAnnouncement(announcement);
  });

  // Phase 4: Get the peer into DETACHED_READY_TO_JOIN via DFP path.
  // Follows the ActivateDFPTransitionsViaIsDFPPath pattern.
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRib1;
    adjRib->setPeerState(PeerUpdateState::DETACHED_RUNNING);

    // Clear the dummy consumer set by PeerManagerTestFixture::setupAdjRib
    // so registerDetachedConsumer can create and register a proper one.
    adjRib->changeListConsumer_.reset();

    // Register detached consumer at the group consumer's position.
    // joinConsumer ensures the per-peer consumer inherits the group's
    // position, so entries published before the group consumer (Phase 1)
    // will NOT have the per-peer bit set.
    adjRib->registerDetachedConsumer(
        ctx.peerMgr->changeListTracker_,
        groupConsumer,
        addPathBitmap,
        nonAddPathBitmap);
    EXPECT_NE(adjRib->changeListConsumeTimer_, nullptr);

    // Set up DFP conditions:
    //   1. Peer PL empty (default)
    //   2. Matching lastSeenRibVersion between group and peer (both 0)
    //   3. Group PL non-empty
    auto group = adjRib->getUpdateGroup();
    auto attrs = std::make_shared<BgpPath>(BgpPathFields());
    attrs->setLocalPref(100);
    std::shared_ptr<const BgpPath> constAttrs = attrs;
    group->tryUpdateAttrToPrefixMapForGroup(
        std::make_pair(
            folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24},
            kPlaceholderPathID),
        nullptr,
        constAttrs);

    EXPECT_TRUE(adjRib->isDFP());

    // Activate detached mode — schedules sendBgpUpdates on the evb
    adjRib->activateDetachedModeProcessing();
  });

  // Phase 5: Pump the evb so sendBgpUpdates runs. isDFP() triggers
  // transition to DETACHED_READY_TO_JOIN with cancelled packing timers.
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRib1;
    EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);
    EXPECT_TRUE(adjRib->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER));
    EXPECT_FALSE(adjRib->changeListConsumeTimer_->isScheduled());
  });

  // Phase 6: Call processRibDumpReq — walks the ShadowRib, calls
  // processRibMessage for each entry, then activateChangeListConsumer
  // triggers scheduleSendBgpUpdates -> sendBgpUpdates ->
  // reschedulePackingTimers.
  evb.runInEventBaseThreadAndWait([&]() {
    auto& adjRib = ctx.adjRib1;
    folly::coro::blockingWait(ctx.peerMgr->processRibDumpReqCoro(
        RibDumpReq(adjRib->getRemotePeerId(), false /* sendAddPath */)));

    // Verify 5 entries from the RibDump were received into the PL.
    // The 6th prefix (10.1.0.0/24) is on the changelist for this consumer
    // and is skipped by processRibDumpReq.
    size_t totalPrefixes = 0;
    for (const auto& [key, prefixes] : adjRib->attrToPrefixMap_) {
      totalPrefixes += prefixes.size();
    }
    EXPECT_EQ(totalPrefixes, 5);
  });

  // Phase 7: EventBase processes sendBgpUpdates which drains the PL
  // and reschedules packing timers.
  evb.runInEventBaseThreadAndWait([&]() {
    EXPECT_TRUE(ctx.adjRib1->changeListConsumeTimer_->isScheduled());
  });

  // Clean up before teardown
  evb.runInEventBaseThreadAndWait([&]() {
    ctx.adjRib1->deactivateDetachedModeProcessing();
    groupConsumer->resetBitmap();
    groupConsumer->terminate();
    groupConsumer->deregisterFromTracker();
    ctx.adjRib1->getUpdateGroup()->resetChangeListConsumer();
    // Clear shadowRibEntries_ while changeListTracker_ is still alive,
    // so ChangeItems properly unlink from the change list.
    ctx.peerMgr->shadowRibEntries_.clear();
  });
  // Drain any async work triggered by cleanup
  evb.runInEventBaseThreadAndWait([]() {});

  tearDown(ctx);
}

class SinglePeerPolicyReEvaluation : public ::testing::Test {
 protected:
  void SetUp() override {
    folly::SingletonVault::singleton()->registrationComplete();
    evb_ = std::make_unique<folly::EventBase>();
    sourceGroup_ = std::make_shared<AdjRibOutGroup>(*evb_, "source_group");
    targetGroup_ = std::make_shared<AdjRibOutGroup>(*evb_, "target_group");

    auto peerId = nettools::bgplib::BgpPeerId(
        folly::IPAddress("10.0.0.1"),
        folly::IPAddressV4("255.0.0.1").toLongHBO());
    adjRib_ = std::make_shared<AdjRib>(
        peerId,
        PeeringParams(),
        *evb_,
        ribInQ_,
        observerQ_,
        std::make_shared<folly::coro::Baton>(),
        nullptr,
        std::make_shared<std::atomic<bool>>(false));
    adjRib_->adjRibOutGroup_ = sourceGroup_;
    sourceGroup_->registerPeer(adjRib_);
  }

  void TearDown() override {
    sourceGroup_->unregisterPeer(adjRib_);
    adjRib_.reset();
    sourceGroup_.reset();
    targetGroup_.reset();
    evb_.reset();
  }

  std::unique_ptr<folly::EventBase> evb_;
  std::shared_ptr<AdjRibOutGroup> sourceGroup_;
  std::shared_ptr<AdjRibOutGroup> targetGroup_;
  std::shared_ptr<AdjRib> adjRib_;
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  MonitoredMPMCQueue<AdjRib::ObservableMessageT> observerQ_;
};
} // namespace facebook::bgp
