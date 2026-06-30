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

#pragma once

/*
 * Unit-test fixture for single peer policy re-evaluation tests.
 *
 * PeerManager_TEST_FRIENDS and AdjRib_TEST_FRIENDS MUST be defined
 * before including this header to grant friend access.
 *
 * Provides:
 *   - PeerManager with update groups enabled
 *   - Two AdjRibs in the same group (same peerGroupName)
 *   - 100 routes pre-loaded in shadowRibEntries_ with 10 attribute sets
 *   - Real routing policies (propagate-all + re-eval-tag)
 *   - Helpers to manipulate peer state and trigger policy re-eval
 */

#include <folly/coro/BlockingWait.h>
#include <folly/coro/GtestHelpers.h>
#include <folly/coro/Task.h>
#include <folly/coro/Timeout.h>
#include <gtest/gtest.h>

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/BgpServiceBase.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ChangeTracker.h"
#include "neteng/fboss/bgp/cpp/changeTracker/TrackableObject.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/facebook/BgpServiceBB.h"
#include "neteng/fboss/bgp/cpp/lib/fibers/FiberBgpPeerManager.h"
#include "neteng/fboss/bgp/cpp/peer/SessionManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/rib/RibBase.h"
#include "neteng/fboss/bgp/cpp/tests/PeerManagerTestUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"

namespace facebook::bgp {

/* ========= TEST FIXTURE CONSTANTS ========= */

/* ---- Policy community constants ---- */
inline constexpr auto kPCommNoAdvt = "65535:65282";
inline constexpr auto kPCommModify = "65500:100";
inline constexpr auto kPCommAppend = "65500:200";

/* ---- Policy name constants ---- */
inline constexpr auto kPNameMatchNoAdvtDeny =
    "match-no-advt-community-deny-continue";
inline constexpr auto kPNameMatchModifyAppend =
    "match-modify-community-append-tag-continue";

/* ---- Route parameterization constants ---- */
inline constexpr int kRouteCount = 100;
inline constexpr int kAttrSetCount = 10;

class UpdateGroupPolicyReEvalUTBase : public PeerManagerTestFixture {
 protected:
  using BgpPeerId = nettools::bgplib::BgpPeerId;

  struct TestContext {
    std::shared_ptr<PeerManager> peerMgr;
    std::shared_ptr<SessionManager> sessionMgr;
    std::shared_ptr<ConfigManager> configMgr;
    folly::F14FastMap<BgpPeerId, std::shared_ptr<AdjRib>> adjRibs;
    std::thread peerMgrThread;
    std::thread sessionMgrThread;

    // Queues created by triggerPeerUpOnEvb that need cleanup in tearDown
    std::vector<std::shared_ptr<AdjRib::AdjRibInQueueT>> sessionInQueues;
    std::vector<std::shared_ptr<AdjRib::BoundedAdjRibOutQueueT>>
        sessionBoundedOutQueues;

    int ribBaseVersion{0};
  };

  std::shared_ptr<AdjRib> setupAdjRibWithPeerGroup(
      folly::EventBase& evb,
      const nettools::bgplib::BgpPeerId& peerId,
      const AsNum& remoteAs,
      std::shared_ptr<folly::coro::Baton>& sessionTerminateBaton,
      const std::shared_ptr<ConfigManager>& configManager,
      const std::shared_ptr<PolicyManager>& policyManager,
      const std::string& description,
      const std::string& peerGroupName) {
    auto config = configManager->getConfig();
    std::optional<std::string> egressPolicyName;
    const auto& peerGroups = config->getPeerGroups();
    auto pgIt = peerGroups.find(peerGroupName);
    if (pgIt != peerGroups.end() &&
        pgIt->second.egress_policy_name().has_value()) {
      egressPolicyName = *pgIt->second.egress_policy_name();
    }

    auto adjRibOutGroup = std::make_shared<AdjRibOutGroup>(evb, peerGroupName);
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
        policyManager,
        isSafeModeOn_,
        /*ingressPolicyName=*/std::nullopt,
        /*egressPolicyName=*/egressPolicyName,
        adjRibOutGroup,
        std::nullopt,
        configManager);
    adjRib->peeringParams_.description = description;
    adjRib->peeringParams_.peerGroupName = peerGroupName;
    adjRib->adjRibOutQueue_ = std::make_shared<AdjRib::AdjRibOutQueueT>();
    adjRib->boundedAdjRibOutQueue_ =
        std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(1000, 800, 50);
    adjRib->pathIdGenerator_ = std::make_unique<PathIdGenerator>(false);
    adjRib->enableEgressQueueBackpressure(true);
    adjRib->isAfiIpv4Negotiated_ = true;
    adjRib->markStateEstablished();
    return adjRib;
  }

  /*
   * Denies routes with kPCommNoAdvt community, accepts everything else.
   */
  static bgp_policy::BgpPolicyStatement buildMatchNoAdvtDenyPolicy() {
    auto match = createBgpPolicyAtomicMatch(
        bgp_policy::BgpPolicyAtomicMatchType::COMMUNITY_LIST, {kPCommNoAdvt});
    auto deny = createBgpPolicyAction(bgp_policy::BgpPolicyActionType::DENY);
    auto denyTerm = createBgpPolicyTerm(
        "deny-no-advt",
        "",
        {std::move(match)},
        {std::move(deny)},
        bgp_policy::FlowControlAction::NEXT_TERM);

    auto accept =
        createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT);
    auto acceptTerm = createBgpPolicyTerm(
        "accept-all",
        "",
        {},
        {std::move(accept)},
        bgp_policy::FlowControlAction::NEXT_TERM);

    return createBgpPolicyStatement(
        kPNameMatchNoAdvtDeny, {std::move(denyTerm), std::move(acceptTerm)});
  }

  /*
   * Appends kPCommAppend to routes with kPCommModify, accepts all.
   */
  static bgp_policy::BgpPolicyStatement buildMatchModifyAppendPolicy() {
    auto match = createBgpPolicyAtomicMatch(
        bgp_policy::BgpPolicyAtomicMatchType::COMMUNITY_LIST, {kPCommModify});
    auto append = createBgpPolicyCommunityAction(
        bgp_policy::CommunityActionType::ADD, {kPCommAppend});
    auto permit =
        createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT);
    auto tagTerm = createBgpPolicyTerm(
        "append-tag",
        "",
        {std::move(match)},
        {std::move(append), std::move(permit)},
        bgp_policy::FlowControlAction::NEXT_TERM);

    auto accept =
        createBgpPolicyAction(bgp_policy::BgpPolicyActionType::PERMIT);
    auto acceptTerm = createBgpPolicyTerm(
        "accept-all",
        "",
        {},
        {std::move(accept)},
        bgp_policy::FlowControlAction::NEXT_TERM);

    return createBgpPolicyStatement(
        kPNameMatchModifyAppend, {std::move(tagTerm), std::move(acceptTerm)});
  }

  static bgp_policy::BgpPolicies buildPolicies() {
    bgp_policy::BgpPolicies policies;
    policies.bgp_policy_statements()->emplace_back(
        buildMatchNoAdvtDenyPolicy());
    policies.bgp_policy_statements()->emplace_back(
        buildMatchModifyAppendPolicy());
    return policies;
  }

  /*
   * Populate 100 routes in shadowRibEntries_.
   * Routes: 100.0.0.0/24 through 100.0.99.0/24.
   * Attribute sets cycle every kAttrSetCount routes via AS_PATH length.
   * Odd-numbered routes are tagged with kPCommNoAdvt.
   * Routes at multiples of 3 are tagged with kPCommModify.
   * A route can carry both communities (e.g. route 3, 9, 15, ...).
   *
   * ribBaseVersion: determines the rib version range and localPref.
   *   ribVersion = ribBaseVersion * 100 + i + 1 (range [base*100+1,
   * base*100+100]). localPref = ribBaseVersion. Use ribBaseVersion=0 for the
   * first batch, ribBaseVersion=1 for the next, etc. to produce non-overlapping
   * version ranges and distinguish batches by localPref.
   *
   * isInitialDump: sets announcement.initialDump.
   *   true = routes are part of the initial RIB dump (default).
   *   false = routes are incremental updates after initialization.
   *
   * routeIndexPredicate: only routes where the predicate returns true
   *   for the route index (0..99) are included in the announcement.
   *   Default: all routes included.
   */
  void publishRouteUpdates(
      TestContext& ctx,
      bool isInitialDump = true,
      std::function<bool(int)> routeIndexPredicate = [](int) { return true; }) {
    int ribBaseVersion = ctx.ribBaseVersion++;
    XLOG(INFO) << "publishRouteUpdates ribBaseVersion " << ribBaseVersion;
    auto& evb = ctx.peerMgr->getEventBase();

    evb.runInEventBaseThreadAndWait([&]() {
      TinyPeerInfo peer(
          kPeerAddr2, kAsn1, kPeerRouterId2, BgpSessionType::EBGP, false);

      RibOutAnnouncement announcement;
      announcement.initialDump = isInitialDump;

      for (int i = 0; i < kRouteCount; ++i) {
        if (!routeIndexPredicate(i)) {
          continue;
        }
        int asPathLen = (i % kAttrSetCount) + 1;
        auto fields = buildBgpPathFields(asPathLen, 0, 0, 0);
        {
          auto mutableAttrs = fields->attrs.get();
          mutableAttrs.localPref = ribBaseVersion;
          fields->attrs = std::move(mutableAttrs);
        }

        nettools::bgplib::BgpAttrCommunitiesC communities;
        if (i % 2 == 1) {
          communities.push_back(
              *nettools::bgplib::BgpAttrCommunityC::createBgpAttrCommunity(
                  kPCommNoAdvt));
        }
        if (i % 3 == 0) {
          communities.push_back(
              *nettools::bgplib::BgpAttrCommunityC::createBgpAttrCommunity(
                  kPCommModify));
        }
        if (!communities.empty()) {
          auto mutableAttrs = fields->attrs.get();
          mutableAttrs.communities = std::move(communities);
          fields->attrs = std::move(mutableAttrs);
        }

        auto attrs = std::make_shared<BgpPath>(*fields);
        attrs->publish();

        announcement.entries.emplace_back(
            folly::CIDRNetwork{
                folly::IPAddress(fmt::format("100.0.{}.0", i)), 24},
            kDefaultPathID,
            peer,
            attrs);
        announcement.entries.back().ribVersion = ribBaseVersion * 100 + i + 1;
      }

      ctx.peerMgr->handleShadowRibEntryAnnouncement(announcement);
    });
  }

  void expectEventualStateOnEvb(
      TestContext& ctx,
      const BgpPeerId& peerId,
      PeerUpdateState expected) {
    auto& evb = ctx.peerMgr->getEventBase();
    WITH_RETRIES({
      auto state = folly::via(&evb, [&]() {
                     return ctx.adjRibs.at(peerId)->getPeerState();
                   }).get();
      EXPECT_EVENTUALLY_EQ(state, expected);
    });
  }

  void drainOne(TestContext& ctx, const BgpPeerId& peerId) {
    auto queue = ctx.adjRibs.at(peerId)->boundedAdjRibOutQueue_;
    XLOGF(
        INFO,
        "drainOne for peer {}: queue size={}",
        peerId.peerAddr.str(),
        queue->size());
    /*
     * Pop one item, but don't block forever if the peer never pushes another
     * (e.g. it already caught up to the group). Time out after a short budget
     * so a logic problem surfaces as a quick failure instead of a hang.
     */
    auto result = folly::coro::blockingWait(
        folly::coro::co_awaitTry(
            folly::coro::timeout(queue->pop(), std::chrono::seconds(2))));
    if (result.hasException()) {
      XLOGF(
          INFO,
          "drainOne for peer {}: timed out, nothing to pop",
          peerId.peerAddr.str());
    }
  }

  void drainQueue(TestContext& ctx, const BgpPeerId& peerId) {
    auto queue = ctx.adjRibs.at(peerId)->boundedAdjRibOutQueue_;
    auto initialSize = queue->size();
    size_t popped = 0;
    while (!queue->empty()) {
      folly::coro::blockingWait(queue->pop());
      ++popped;
    }
    XLOGF(
        INFO,
        "drainQueue for peer {}: initial size={}, popped={}",
        peerId.peerAddr.str(),
        initialSize,
        popped);
  }

  void resizePeerQueue(
      const std::shared_ptr<AdjRib>& adjRib,
      size_t capacity,
      size_t hiWm,
      size_t loWm) {
    adjRib->boundedAdjRibOutQueue_ =
        std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(capacity, hiWm, loWm);
  }

  /*
   * Block the group by shrinking a joined peer's queue. The caller
   * should publish route updates afterwards to trigger distribution
   * and fill the small queue, causing the peer to become JOINED_BLOCKED
   * and the group to enter WAITING.
   * Slow peer thresholds are set very lenient to prevent detachment.
   */
  void blockGroupViaPeerOnEvb(
      TestContext& ctx,
      const BgpPeerId& blockedPeerId) {
    auto& evb = ctx.peerMgr->getEventBase();

    evb.runInEventBaseThreadAndWait([&]() {
      auto& adjRib = ctx.adjRibs.at(blockedPeerId);
      resizePeerQueue(adjRib, 10, 8, 2);

      auto group = adjRib->getUpdateGroup();
      UpdateGroupConfig cfg;
      cfg.allowSlowPeerDetach = true;
      cfg.slowPeerTimeThreshold = std::chrono::milliseconds(50000000);
      cfg.slowPeerBlockCountThreshold = 100000;
      cfg.slowPeerBlockCountWindow = std::chrono::milliseconds(50000000);
      group->setUpdateGroupConfigForTesting(cfg);
    });
  }

  /*
   * Transition a peer from JOINED_BLOCKED to DETACHED_BLOCKED.
   * Requires at least 2 peers in the group (detachSlowPeer skips if
   * numInSyncPeers <= 1).
   *
   * useBlockFrequency=true: frequency-based detection (block count
   *   threshold = 1, triggers on first markPeerBlocked).
   * useBlockFrequency=false: duration-based detection (very short
   *   slowPeerTimeThreshold, triggers after timer fires).
   *
   * Steps:
   *   1. Set all peers in the group to JOINED_RUNNING with sync bitmap
   *   2. Configure slow peer thresholds based on useBlockFrequency
   *   3. Replace the target peer's bounded queue with a small one and fill it
   *   4. markPeerBlocked → threshold hit → detachSlowPeer → DETACHED_BLOCKED
   */
  void triggerDetachedBlockedFromJoinedOnEvb(
      TestContext& ctx,
      const BgpPeerId& targetPeerId,
      bool useBlockFrequency = true) {
    auto& evb = ctx.peerMgr->getEventBase();

    evb.runInEventBaseThreadAndWait([&]() {
      auto& targetAdjRib = ctx.adjRibs.at(targetPeerId);
      auto group = targetAdjRib->getUpdateGroup();
      ASSERT_NE(group, nullptr);
      ASSERT_GE(group->getMemberCount(), 2)
          << "Need at least 2 peers for frequency detachment";

      for (auto& [peerId, adjRib] : ctx.adjRibs) {
        if (adjRib->getUpdateGroup().get() == group.get()) {
          adjRib->setPeerState(PeerUpdateState::JOINED_RUNNING);
          group->setSyncBitForTesting(adjRib->getGroupBitPosition());
        }
      }

      UpdateGroupConfig cfg;
      cfg.allowSlowPeerDetach = true;
      if (useBlockFrequency) {
        cfg.slowPeerTimeThreshold = std::chrono::milliseconds(600000);
        cfg.slowPeerBlockCountThreshold = 1;
        cfg.slowPeerBlockCountWindow = std::chrono::milliseconds(60000);
      } else {
        cfg.slowPeerTimeThreshold = std::chrono::milliseconds(1);
        cfg.slowPeerBlockCountThreshold = 100000;
        cfg.slowPeerBlockCountWindow = std::chrono::milliseconds(60000);
      }
      group->setUpdateGroupConfigForTesting(cfg);

      auto smallQueue =
          std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(3, 2, 1);
      targetAdjRib->boundedAdjRibOutQueue_ = smallQueue;

      nettools::bgplib::FiberBgpPeer::InputMessageT dummyMsg{
          nettools::bgplib::BgpEndOfRib{}};
      for (int i = 0; i < 2; ++i) {
        smallQueue->push(
            std::optional<nettools::bgplib::FiberBgpPeer::InputMessageT>(
                dummyMsg));
      }
      ASSERT_TRUE(smallQueue->isBlocked());

      group->markPeerBlocked(targetAdjRib);

      EXPECT_EQ(
          targetAdjRib->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
      XLOGF(
          INFO,
          "Peer {} successfully detached from JOINED via {}",
          targetPeerId.peerAddr.str(),
          useBlockFrequency ? "frequency" : "duration");
    });
  }

  /*
   * Transition a peer to DETACHED_BLOCKED via the detached mode path:
   * JOINED_BLOCKED -> DETACHED_BLOCKED -> drain -> DETACHED_RUNNING
   * -> process changelist entries -> queue re-fills -> DETACHED_BLOCKED.
   *
   * Requires setUp + sendInitialRibDump already called.
   * Publishes routes to create changelist entries for the detached peer
   * to process after being unblocked.
   */
  void triggerDetachedBlockedFromDetachedOnEvb(
      TestContext& ctx,
      const BgpPeerId& targetPeerId) {
    auto& evb = ctx.peerMgr->getEventBase();

    // Find the blocker: any other peer in the same group.
    BgpPeerId blockerPeerId = [&]() {
      auto group = ctx.adjRibs.at(targetPeerId)->getUpdateGroup();
      for (auto& [bitPos, adjRib] : group->getBitToAdjRibs()) {
        if (adjRib->getRemotePeerId() != targetPeerId) {
          return adjRib->getRemotePeerId();
        }
      }
      return targetPeerId;
    }();

    /*
     * Step 1: Configure queues and thresholds. Use the same setup
     * as triggerPeerDetachedReadyToJoin to get a real detachment
     * with a running deferredPushToPeer coroutine.
     */
    evb.runInEventBaseThreadAndWait([&]() {
      auto group = ctx.adjRibs.at(targetPeerId)->getUpdateGroup();
      UpdateGroupConfig cfg;
      cfg.allowSlowPeerDetach = true;
      cfg.slowPeerTimeThreshold = std::chrono::milliseconds(50000000);
      cfg.slowPeerBlockCountThreshold = 1;
      cfg.slowPeerBlockCountWindow = std::chrono::milliseconds(60000);
      group->setUpdateGroupConfigForTesting(cfg);

      resizePeerQueue(ctx.adjRibs.at(targetPeerId), 10, 7, 2);
      resizePeerQueue(ctx.adjRibs.at(blockerPeerId), 10, 8, 2);
    });

    // Step 2: Publish routes to trigger detachment.
    publishRouteUpdates(
        ctx, /*isInitialDump=*/false, [](int i) { return i < 20; });
    expectEventualStateOnEvb(
        ctx, targetPeerId, PeerUpdateState::DETACHED_BLOCKED);

    /*
     * After the first detach the blocker is JOINED_BLOCKED and the group is
     * frozen, so the detached peer sits exactly at the group's changelist
     * marker -- a detached peer is "caught up" once its marker == the group's
     * (AdjRib::readyToRejoin, AdjRibOut.cpp), and it never advances past the
     * group -- so it has nothing to process and cannot re-block. To re-block it
     * from detached we must move the group PAST it, freeze the group there,
     * then let the detached peer catch up and refill its own queue.
     */

    /*
     * Step 3: Unblock the group and advance its changelist marker well past the
     * detached peer. Repeatedly drain the blocker (so the group keeps
     * consuming) while publishing fresh changes. The detached target stays
     * frozen at its marker because its queue is full, so the gap between its
     * marker and the group's keeps growing.
     */
    for (int round = 0; round < 5; ++round) {
      drainQueue(ctx, blockerPeerId);
      publishRouteUpdates(ctx, /*isInitialDump=*/false);
    }

    /*
     * Step 4: Re-block the group at the advanced position. Stop draining the
     * blocker and publish once more so its queue fills and the group freezes
     * ahead of the detached peer.
     */
    publishRouteUpdates(ctx, /*isInitialDump=*/false);
    expectEventualStateOnEvb(
        ctx, blockerPeerId, PeerUpdateState::JOINED_BLOCKED);

    /*
     * Step 5: Activate the detached target (drain its queue once to drop below
     * the low-water mark), then leave it alone. As it catches up to the group's
     * advanced marker it refills its small queue past the high-water mark and
     * re-blocks. Do NOT keep draining -- that is what previously kept the queue
     * empty so it never re-filled.
     */
    drainQueue(ctx, targetPeerId);
    expectEventualStateOnEvb(
        ctx, targetPeerId, PeerUpdateState::DETACHED_BLOCKED);
  }

  /*
   * Transition a peer to DETACHED_READY_TO_JOIN.
   *
   * Requires setUp + sendInitialRibDump already called.
   * targetIndex is the peer index to detach. The peer with the next
   * higher bit position in the group becomes the JOINED_BLOCKED
   * blocker that prevents the group from advancing.
   *
   * isDFP=true (Detached Fast Peer):
   *   Group and peer are at the same CL position after the peer
   *   finishes draining. The peer was never behind the group.
   *
   * isDFP=false (Detached Slow Peer):
   *   Additional route updates are published after detachment so the
   *   group moves ahead. The peer catches up by consuming the extra
   *   CL changes.
   */
  void triggerPeerDetachedReadyToJoin(
      TestContext& ctx,
      const BgpPeerId& targetPeerId,
      bool isDFP = true) {
    auto& evb = ctx.peerMgr->getEventBase();

    // Find the blocker: any other peer in the same group.
    BgpPeerId blockerPeerId = [&]() {
      auto group = ctx.adjRibs.at(targetPeerId)->getUpdateGroup();
      for (auto& [bitPos, adjRib] : group->getBitToAdjRibs()) {
        if (adjRib->getRemotePeerId() != targetPeerId) {
          return adjRib->getRemotePeerId();
        }
      }
      return targetPeerId;
    }();

    /*
     * Step 1: Configure thresholds.
     * Frequency threshold = 1: first peer to block gets detached.
     * The target peer has a smaller queue than the blocker, so it
     * blocks first during distribution.
     */
    evb.runInEventBaseThreadAndWait([&]() {
      auto group = ctx.adjRibs.at(targetPeerId)->getUpdateGroup();
      UpdateGroupConfig cfg;
      cfg.allowSlowPeerDetach = true;
      cfg.slowPeerTimeThreshold = std::chrono::milliseconds(50000000);
      cfg.slowPeerBlockCountThreshold = 1;
      cfg.slowPeerBlockCountWindow = std::chrono::milliseconds(60000);
      group->setUpdateGroupConfigForTesting(cfg);

      // Target: hiWm=6 so it blocks before the blocker (hiWm=7)
      resizePeerQueue(ctx.adjRibs.at(targetPeerId), 10, 6, 0);
      if (isDFP) {
        resizePeerQueue(ctx.adjRibs.at(blockerPeerId), 10, 7, 0);
      } else {
        /*
         * DSP: blocker needs a larger queue so it doesn't block on
         * the first batch, allowing the group to advance on the CL
         * before blocking on the second batch.
         */
        resizePeerQueue(ctx.adjRibs.at(blockerPeerId), 20, 12, 0);
      }
    });

    /*
     * Step 2: Publish routes to trigger distribution.
     * Target peer blocks → frequency detach → DETACHED_BLOCKED.
     */
    publishRouteUpdates(
        ctx, /*isInitialDump=*/false, [](int i) { return i < 20; });

    expectEventualStateOnEvb(
        ctx, targetPeerId, PeerUpdateState::DETACHED_BLOCKED);

    /*
     * Step 3 (DSP only): Publish more routes so the group moves ahead.
     * Wait for the blocker peer to become JOINED_BLOCKED before
     * proceeding, so the group has fully processed the new routes.
     */
    if (!isDFP) {
      publishRouteUpdates(
          ctx, /*isInitialDump=*/false, [](int i) { return i < 12; });
      expectEventualStateOnEvb(
          ctx, blockerPeerId, PeerUpdateState::JOINED_BLOCKED);
    }

    /*
     * Step 4: Drain the target peer's queue so the detached peer can
     * finish its packing list.
     */
    drainQueue(ctx, targetPeerId);

    /*
     * Step 4b (DSP only): The detached peer receives new changelist
     * entries from step 3 and re-blocks. Drain again.
     */
    if (!isDFP) {
      expectEventualStateOnEvb(
          ctx, targetPeerId, PeerUpdateState::DETACHED_BLOCKED);
      drainQueue(ctx, targetPeerId);
    }

    // Step 5: Wait for the peer to reach DETACHED_READY_TO_JOIN.
    expectEventualStateOnEvb(
        ctx, targetPeerId, PeerUpdateState::DETACHED_READY_TO_JOIN);
  }

  /*
   * Bring up a new peer into DETACHED_INIT_DUMP state.
   *
   * Precondition: the group must be initialized (not UNINITIALIZED).
   * Call setUp(2) + sendInitialRibDump first.
   *
   * Steps:
   *   1. Set testOnlyDeferInitDump on the new peer's address so the
   *      RibDumpReq is re-buffered and the peer stays in
   *      DETACHED_INIT_DUMP.
   *   2. Bring up the peer via triggerPeerUpOnEvb with large queues
   *      (capacity=100, hiWm=80, loWm=10).
   *   3. Wait for the peer to reach DETACHED_INIT_DUMP.
   *
   * Returns the new peer's BgpPeerId.
   */
  BgpPeerId triggerDetachedInitDump(TestContext& ctx, int peerIndex) {
    auto peerId = makePeerId(peerIndex);
    auto& evb = ctx.peerMgr->getEventBase();

    triggerPeerDownOnEvb(ctx, peerId);
    expectEventualStateOnEvb(ctx, peerId, PeerUpdateState::DOWN);

    ctx.peerMgr->testOnlySetDeferInitDump(peerId.peerAddr, true);
    triggerPeerUpOnEvb(ctx, peerId, 100, 80, 10);

    /*
     * sessionEstablished creates a new AdjRib. Refresh ctx.adjRibs
     * so subsequent checks use the new AdjRib, not the stale one.
     */
    evb.runInEventBaseThreadAndWait([&]() {
      auto it = ctx.peerMgr->adjRibs_.find(peerId);
      if (it != ctx.peerMgr->adjRibs_.end()) {
        ctx.adjRibs[peerId] = it->second;
      }
    });

    expectEventualStateOnEvb(ctx, peerId, PeerUpdateState::DETACHED_INIT_DUMP);

    return peerId;
  }

  /*
   * Simulate a peer session going down by pushing an IDLE event
   * directly to the notifyCoroQueue. PeerManager::processPeerEventLoop
   * processes it as sessionTerminated.
   */
  void triggerPeerDownOnEvb(TestContext& ctx, const BgpPeerId& peerId) {
    nettools::bgplib::FiberBgpPeer::ObservableStateT stateEvt{
        .peerId = peerId,
        .state = nettools::bgplib::BgpSessionState::IDLE,
        .versionNumber = 0,
        .sessionInfo = nullptr};

    nettools::bgplib::ObservableEventT obsEvent = std::move(stateEvt);
    ctx.sessionMgr->getNotifyCoroQueue().push(std::move(obsEvent));
  }

  /*
   * Simulate a peer session coming up by pushing an ESTABLISHED event
   * directly to the notifyCoroQueue. PeerManager::processPeerEventLoop
   * processes it as sessionEstablished.
   */
  void triggerPeerUpOnEvb(
      TestContext& ctx,
      const BgpPeerId& peerId,
      int queueCapacity = 1000,
      int queueHighWm = 800,
      int queueLowWm = 50) {
    auto adjRibOutQ = std::make_shared<AdjRib::AdjRibOutQueueT>();
    auto boundedAdjRibOutQ = std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(
        queueCapacity, queueHighWm, queueLowWm);
    auto adjRibInQ = std::make_shared<AdjRib::AdjRibInQueueT>();
    auto versionNumber = std::make_shared<nettools::bgplib::VersionNumber>(1);

    nettools::bgplib::BgpPeerDisplayInfo displayInfo;
    displayInfo.peeringParams.peerAddr = peerId.peerAddr;
    displayInfo.peeringParams.remoteAs = kAsn1;
    displayInfo.peeringParams.localAs = kAsn1;
    displayInfo.peeringParams.globalAs = kAsn1;
    displayInfo.peeringParams.isAfiIpv4Configured = AfiIpv4Configured{true};
    displayInfo.peeringParams.isAfiIpv6Configured = AfiIpv6Configured{true};
    displayInfo.remoteBgpId = peerId.remoteBgpId;
    displayInfo.negotiatedCapabilities.mpExtV4Unicast() = true;
    displayInfo.negotiatedCapabilities.as4byte() = true;

    auto sessionInfo = nettools::bgplib::FiberBgpPeer::getObservableSessionInfo(
        displayInfo, adjRibOutQ, boundedAdjRibOutQ, adjRibInQ, versionNumber);

    nettools::bgplib::FiberBgpPeer::ObservableStateT stateEvt{
        .peerId = peerId,
        .state = nettools::bgplib::BgpSessionState::ESTABLISHED,
        .versionNumber = versionNumber->getWithoutLock(),
        .sessionInfo = std::move(sessionInfo)};

    nettools::bgplib::ObservableEventT obsEvent = std::move(stateEvt);
    ctx.sessionMgr->getNotifyCoroQueue().push(std::move(obsEvent));

    ctx.sessionInQueues.push_back(adjRibInQ);
    ctx.sessionBoundedOutQueues.push_back(boundedAdjRibOutQ);
  }

  /*
   * Update a peer's egress policy via BgpServiceBase::setPeersPolicy.
   * This updates the config and the AdjRib's egressPolicyName_.
   */
  void updatePeerEgressPolicyOnEvb(
      TestContext& ctx,
      const BgpPeerId& peerId,
      const std::string& newPolicyName) {
    static std::aligned_storage_t<sizeof(RibBase), alignof(RibBase)> ribBuf;
    static std::aligned_storage_t<sizeof(Watchdog), alignof(Watchdog)>
        watchdogBuf;
    auto bgpService = std::make_shared<BgpServiceBB>(
        *ctx.peerMgr,
        ctx.configMgr,
        reinterpret_cast<RibBase&>(ribBuf),
        reinterpret_cast<Watchdog&>(watchdogBuf),
        /*nlWrapper=*/nullptr,
        /*enable_thrift_protection=*/false);
    auto peersPolicy = std::make_unique<
        std::map<std::string, std::map<bgp_policy::DIRECTION, std::string>>>();
    (*peersPolicy)[peerId.peerAddr.str()][bgp_policy::DIRECTION::OUT] =
        newPolicyName;
    /*
     * BgpServiceBB implements the coroutine variant (co_setPeersPolicy); the
     * sync setPeersPolicy is the unimplemented generated stub. Drive the coro
     * on the peer manager's evb, blocking the test thread for the result.
     */
    auto& evb = ctx.peerMgr->getEventBase();
    auto result = folly::coro::blockingWait(
        bgpService->co_setPeersPolicy(std::move(peersPolicy)).scheduleOn(&evb));
    ASSERT_EQ(
        result,
        neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  }

  /*
   * Remove the per-peer egress policy override so the peer falls back to its
   * peer-group policy (peerOverride becomes false). setPeersPolicy installs a
   * per-peer override even when the name matches the group's, so unsetting is
   * the only way to clear the override.
   */
  void unsetPeerEgressPolicyOnEvb(TestContext& ctx, const BgpPeerId& peerId) {
    static std::aligned_storage_t<sizeof(RibBase), alignof(RibBase)> ribBuf;
    static std::aligned_storage_t<sizeof(Watchdog), alignof(Watchdog)>
        watchdogBuf;
    auto bgpService = std::make_shared<BgpServiceBB>(
        *ctx.peerMgr,
        ctx.configMgr,
        reinterpret_cast<RibBase&>(ribBuf),
        reinterpret_cast<Watchdog&>(watchdogBuf),
        /*nlWrapper=*/nullptr,
        /*enable_thrift_protection=*/false);
    auto peersToUnset = std::make_unique<
        std::map<std::string, std::set<bgp_policy::DIRECTION>>>();
    (*peersToUnset)[peerId.peerAddr.str()].insert(bgp_policy::DIRECTION::OUT);
    auto& evb = ctx.peerMgr->getEventBase();
    auto result = folly::coro::blockingWait(
        bgpService->co_unsetPeersPolicy(std::move(peersToUnset))
            .scheduleOn(&evb));
    ASSERT_EQ(
        result,
        neteng::fboss::bgp::thrift::BgpPolicyChangeResult::POLICIES_APPLIED);
  }

  static std::function<bool(const AdjRibEntry&, const folly::CIDRNetwork&)>
  verifyAdvertised() {
    return [](const AdjRibEntry& entry, const folly::CIDRNetwork& prefix) {
      EXPECT_NE(entry.getPostAttr(), nullptr)
          << prefix.first.str() << "/" << prefix.second
          << " was not advertised";
      return entry.getPostAttr() != nullptr;
    };
  }

  static std::function<bool(const AdjRibEntry&, const folly::CIDRNetwork&)>
  verifyNotAdvertised() {
    return [](const AdjRibEntry& entry, const folly::CIDRNetwork& prefix) {
      EXPECT_EQ(entry.getPostAttr(), nullptr)
          << prefix.first.str() << "/" << prefix.second
          << " should be withdrawn or not advertised";
      return entry.getPostAttr() == nullptr;
    };
  }

  static std::function<bool(const AdjRibEntry&, const folly::CIDRNetwork&)>
  verifyCommOnAdvertisedRoute(const std::string& community) {
    auto expectedComm =
        *nettools::bgplib::BgpAttrCommunityC::createBgpAttrCommunity(community);
    return [expectedComm](
               const AdjRibEntry& entry,
               const folly::CIDRNetwork& prefix) -> bool {
      auto postPolicy = entry.getPostAttr();
      EXPECT_NE(postPolicy, nullptr) << prefix.first.str() << "/"
                                     << prefix.second << " was not advertised";
      if (!postPolicy) {
        return false;
      }
      const auto& communities = postPolicy->getCommunities();
      bool found = false;
      for (const auto& comm : communities.get()) {
        if (comm == expectedComm) {
          found = true;
          break;
        }
      }
      EXPECT_TRUE(found) << prefix.first.str() << "/" << prefix.second
                         << " missing expected community";
      return found;
    };
  }

  /*
   * Verify a peer's RIB-OUT entries in the group's LiteTree.
   * Returns the count of entries matching routeIndexPredicate.
   * Checks the peer's owner key first; if not found, falls back to the
   * group owner key for shared entries via isEntryNotShared.
   *
   * verifyOnRouteIndexFunc is called on each matched entry for custom
   * verification (e.g., verifyAdvertised, verifyCommOnAdvertisedRoute).
   *
   * Uses folly::via to run on the evb and return the count via Future.
   */
  size_t verifyRibOutEntries(
      TestContext& ctx,
      const BgpPeerId& peerId,
      std::function<bool(int)> routeIndexPredicate = [](int) { return true; },
      std::function<bool(const AdjRibEntry&, const folly::CIDRNetwork&)>
          verifyOnRouteIndexFunc =
              [](const AdjRibEntry&, const folly::CIDRNetwork&) {
                return true;
              }) {
    auto& evb = ctx.peerMgr->getEventBase();

    return folly::via(
               &evb,
               [&]() -> size_t {
                 auto& adjRib = ctx.adjRibs.at(peerId);
                 auto group = adjRib->getUpdateGroup();
                 auto peerOwnerKey = adjRib->getPeerOwnerKey();
                 auto groupOwnerKey = group->getGroupOwnerKey();
                 bool isDetached = adjRib->isDetachedPeer();
                 auto detachedRibVersion = adjRib->getDetachedRibVersion();

                 size_t count = 0;
                 for (auto it = group->LiteTree_.begin();
                      it != group->LiteTree_.end();
                      ++it) {
                   auto& ownerMap = it->value();
                   AdjRibEntry* entry = nullptr;

                   auto peerIt = ownerMap.find(peerOwnerKey);
                   if (peerIt != ownerMap.end()) {
                     entry = peerIt->second.get();
                   } else {
                     auto groupIt = ownerMap.find(groupOwnerKey);
                     if (groupIt != ownerMap.end() &&
                         (!isDetached ||
                          AdjRibOutGroup::isEntryShared(
                              detachedRibVersion,
                              groupIt->second->getRibVersion()))) {
                       entry = groupIt->second.get();
                     }
                   }

                   if (!entry) {
                     continue;
                   }

                   int routeIndexOctet = it.ipAddress().asV4().getNthMSByte(2);
                   if (!routeIndexPredicate(routeIndexOctet)) {
                     continue;
                   }
                   folly::CIDRNetwork prefix{it.ipAddress(), it.masklen()};
                   if (verifyOnRouteIndexFunc(*entry, prefix)) {
                     ++count;
                   }
                 }
                 return count;
               })
        .get();
  }

  /*
   * Generate a BgpPeerId from an index. Peer addresses start at 10.1.0.1.
   */
  static BgpPeerId makePeerId(int index) {
    auto addr = folly::IPAddress(
        fmt::format("10.1.{}.{}", index / 256, (index % 256) + 1));
    return BgpPeerId(addr, addr.asV4().toLongHBO());
  }

  /*
   * Create the full test context:
   *   - PeerManager with update groups + policies
   *   - numPeers AdjRibs in the same group
   *   - 100 routes in shadowRibEntries_
   */
  TestContext setUp(int numPeers = 2, bool initialDumpCompleted = true) {
    auto policies = buildPolicies();

    auto config = getConfig(
        /*includeStaticPeer=*/true,
        /*includeDynamicShivPeer=*/true,
        /*includeDynamicMonitorPeer=*/false,
        /*includeDynamicVipInjectorPeer=*/false,
        /*enableStatefulHa=*/false,
        /*enableVipServer=*/true,
        /*eorTimeS=*/1,
        /*enableSubscriberLimit=*/false,
        /*enableSwitchLimit=*/false,
        /*applyGoldenPrefixPolicy=*/false,
        /*bgpFeatures=*/{},
        /*enableDynamicPolicyEvaluation=*/false,
        /*enableUpdateGroup=*/true);

    auto thriftConfig = config->getConfig();
    thrift::PeerGroup peerGroupA;
    peerGroupA.name() = "PEERGROUP_A";
    peerGroupA.egress_policy_name() = kPNameMatchNoAdvtDeny;
    thriftConfig.peer_groups()->push_back(std::move(peerGroupA));

    for (int i = 0; i < numPeers; ++i) {
      auto addr = makePeerId(i).peerAddr;
      thrift::BgpPeer peer;
      peer.peer_addr() = addr.str();
      peer.local_addr() = kLocalAddr1.str();
      peer.next_hop4() = kV4Nexthop1.str();
      peer.next_hop6() = kV6Nexthop1.str();
      peer.remote_as_4_byte() = kAsn1;
      peer.peer_group_name() = "PEERGROUP_A";
      thriftConfig.peers()->push_back(std::move(peer));
    }

    config = std::make_shared<Config>(std::move(thriftConfig));

    auto configManager = std::make_shared<ConfigManager>(config);
    auto globalConfig = config->getBgpGlobalConfig();
    auto policyManager =
        std::make_shared<PolicyManager>(policies, globalConfig.get());
    auto peerMgr = std::make_shared<PeerManager>(
        configManager, policyManager, ribInQ_, ribOutQ_, nbrRouteChangeQ_);

    auto sessionMgr = std::make_shared<SessionManager>(*globalConfig, false);
    peerMgr->setSessionManager(sessionMgr);

    auto& evb = peerMgr->getEventBase();

    folly::F14FastMap<BgpPeerId, std::shared_ptr<AdjRib>> adjRibs;
    for (int i = 0; i < numPeers; ++i) {
      auto peerId = makePeerId(i);
      auto baton = std::make_shared<folly::coro::Baton>();
      peerMgr->sessionTerminateBatons_.insert_or_assign(peerId, baton);
      auto adjRib = setupAdjRibWithPeerGroup(
          evb,
          peerId,
          AsNum(kAsn1),
          baton,
          configManager,
          policyManager,
          fmt::format("adjRib{}", i),
          "PEERGROUP_A");
      /*
       * Post immediately: in production the baton is posted when the
       * AdjRib's processing loop finishes. Since we never start that
       * loop for manually created peers, we post it here so that
       * sessionEstablished's waitForSessionTerminateBaton doesn't hang.
       */
      baton->post();
      adjRib->setInInitialAnnouncement();
      adjRib->buildAndSetUpdateGroupKey();

      auto group = peerMgr->updateGroupManager_->findOrCreateGroup(
          adjRib->getUpdateGroupKey());
      adjRib->adjRibOutGroup_ = group;
      group->registerPeer(adjRib);
      peerMgr->adjRibs_[peerId] = adjRib;
      adjRibs[peerId] = std::move(adjRib);
    }

    /*
     * Wire each group's change list tracker/bitmaps. The group consumer is
     * registered later by the initial dump (buildAndScheduleSendInitialDump-
     * FromShadowRib), so we don't register it here to avoid
     * double-registration.
     */
    auto changeListTracker = peerMgr->getChangeListTracker();
    for (auto& [key, group] : peerMgr->updateGroupManager_->getAllGroups()) {
      group->setChangeListTracker(
          changeListTracker,
          peerMgr->addPathConsumerBitmap_,
          peerMgr->nonAddPathConsumerBitmap_);
    }

    peerMgr->ribInitialAnnouncementStarted_ = true;
    peerMgr->ribInitialAnnouncementDone_ = true;
    peerMgr->ribInitPathComputationNotified_ = true;
    peerMgr->initialized_ = true;

    auto peerMgrThread = peerMgr->runInThread();
    auto sessionMgrThread = sessionMgr->runInThread();

    TestContext ctx{
        peerMgr,
        sessionMgr,
        configManager,
        std::move(adjRibs),
        std::move(peerMgrThread),
        std::move(sessionMgrThread)};

    if (initialDumpCompleted) {
      publishRouteUpdates(ctx);
    }

    return ctx;
  }

  void sendInitialRibDump(TestContext& ctx) {
    auto& evb = ctx.peerMgr->getEventBase();
    // Snapshot the group pointers on the evb thread for safe map access.
    std::vector<std::shared_ptr<AdjRibOutGroup>> groups;
    evb.runInEventBaseThreadAndWait([&]() {
      for (auto& [key, group] :
           ctx.peerMgr->updateGroupManager_->getAllGroups()) {
        groups.push_back(group);
      }
    });
    /*
     * Drive each dump Task on the evb but block the *test* thread (not the evb
     * loop) for completion, so the evb stays free to make progress while the
     * Task awaits scope/timer work bound to it.
     */
    for (const auto& group : groups) {
      folly::coro::blockingWait(
          group->buildAndScheduleSendInitialDumpFromShadowRib().scheduleOn(
              &evb));
    }
  }

  void tearDown(TestContext& ctx) {
    XLOG(INFO, "START tearDown");
    auto& evb = ctx.peerMgr->getEventBase();

    evb.runInEventBaseThreadAndWait([&]() {
      for (auto& [peerId, baton] : ctx.peerMgr->sessionTerminateBatons_) {
        if (baton && !baton->ready()) {
          baton->post();
        }
      }

      /*
       * Cancel async scopes on all AdjRibs so blocked coroutines
       * (e.g. deferredPushToPeer waiting on waitToPush) are cancelled.
       */
      for (auto& [peerId, adjRib] : ctx.adjRibs) {
        if (adjRib->asyncScope_) {
          adjRib->asyncScope_->requestCancellation();
        }
      }
    });

    // Drain and close all peer queues.
    for (auto& [peerId, adjRib] : ctx.adjRibs) {
      drainQueue(ctx, peerId);
      auto queue = adjRib->boundedAdjRibOutQueue_;
      if (queue) {
        queue->close();
      }
    }

    for (auto& inQ : ctx.sessionInQueues) {
      if (inQ) {
        inQ->fiberPush(nettools::bgplib::FiberBgpPeer::BgpSessionStop{});
      }
    }
    for (auto& outQ : ctx.sessionBoundedOutQueues) {
      if (outQ) {
        outQ->close();
      }
    }

    ctx.peerMgr->stop();
    ctx.sessionMgr->stop();
    ctx.peerMgrThread.join();
    ctx.sessionMgrThread.join();

    XLOG(INFO, "END tearDown");
  }
};

} // namespace facebook::bgp
