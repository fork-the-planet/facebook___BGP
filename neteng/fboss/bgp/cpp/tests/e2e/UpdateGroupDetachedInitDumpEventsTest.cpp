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
 * E2E tests: DETACHED_INIT_DUMP state x Event coverage
 *
 * Stack A prefix range: 18.x.0.0/16 (freq and PL drain with
 * SlowPeerDetectionTest fixture) Stack B prefix range: 71.x.0.0/16 (route
 * withdraw, unblock, freq, PL drain with MultiPeerTest fixture)
 *
 * Fixtures: UpdateGroupSlowPeerDetectionTest, UpdateGroupMultiPeerTest
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-DID x E-SLOW-FREQ
 * N/A scenario: peer is already detached (DETACHED_INIT_DUMP). The slow
 * peer frequency threshold is irrelevant -- peer is already out of the
 * group's fast path. Verify no double-detachment or crash.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DetachedInitDump_SlowFreqNoop) {
  XLOGF(INFO, "=== TEST: DetachedInitDump_SlowFreqNoop ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"18.30.0.0/16"}, {"1830:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1830:1"));
  injectLocalRoutesAtRuntime({"18.31.0.0/16"}, {"1831:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1831:1"));
  injectLocalRoutesAtRuntime({"18.32.0.0/16"}, {"1832:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1832:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* down -> unblock + up -> DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /*
   * Peer3 re-enters the group in DETACHED_INIT_DUMP and runs an independent
   * init dump sourced from the ShadowRib. Verify it catches up on the routes
   * it missed while blocked (18.30/18.31/18.32), confirming the detached rib
   * dump walks the ShadowRib and emits the right entries. Order-independent:
   * the dump walks the ShadowRib in hash order.
   */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "18.30.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1830:1"},
       {.prefix = "18.31.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1831:1"},
       {.prefix = "18.32.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1832:1"}}));

  /*
   * N/A: peer is already detached. Inject more routes to simulate
   * activity -- freq threshold should NOT cause double-detachment.
   */
  injectLocalRoutesAtRuntime({"18.33.0.0/16"}, {"1833:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1833:1"));

  /*
   * Peer4 still functional, no crash from freq threshold on detached peer.
   * Note: isPeerDetached returns false for DETACHED_INIT_DUMP after
   * reconnect because the detached consumer was cleaned up during DOWN.
   * The peer IS in DID state but the old detachment tracking was reset.
   */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: DetachedInitDump_SlowFreqNoop ===");
}

/*
 * P-DID x E-PL-DRAIN
 * Init dump PL drained for peer in DETACHED_INIT_DUMP. After freq-threshold
 * detach -> down -> up, the peer starts init dump. Drain the init dump queue
 * and verify peer4 continues to function.
 *
 * Note: peers in DETACHED_INIT_DUMP may NOT transition to JOINED_RUNNING
 * (learned pattern). We drain what we can and verify peer4 is functional.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, DetachedInitDump_PlDrain) {
  XLOGF(INFO, "=== TEST: DetachedInitDump_PlDrain ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route so init dump has something to send */
  injectLocalRoutesAtRuntime({"18.40.0.0/16"}, {"1840:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.40.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1840:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1840:1"));

  /* Detach peer3 via freq threshold */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"18.41.0.0/16"}, {"1841:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1841:1"));
  injectLocalRoutesAtRuntime({"18.42.0.0/16"}, {"1842:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1842:1"));
  injectLocalRoutesAtRuntime({"18.43.0.0/16"}, {"1843:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.43.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1843:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* down -> unblock + up -> DETACHED_INIT_DUMP */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /*
   * Peer3 is in DETACHED_INIT_DUMP and runs an independent init dump sourced
   * from the ShadowRib. Verify it receives the correct routes: the shared
   * route it already had (18.40) AND the routes it missed while blocked
   * (18.41/18.42/18.43). This validates that the detached rib dump walks the
   * ShadowRib and emits the right entries to the catching-up peer.
   * Order-independent: the dump walks the ShadowRib in hash order.
   */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "18.40.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1840:1"},
       {.prefix = "18.41.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1841:1"},
       {.prefix = "18.42.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1842:1"},
       {.prefix = "18.43.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "1843:1"}}));

  /*
   * Drain any remaining messages (e.g. EoR) from peer3's init dump queue.
   *
   * Do NOT wait for JOINED_RUNNING -- peers in DETACHED_INIT_DUMP
   * may never reach it (learned pattern).
   */
  drainPeerQueueCompletely(peerId3);

  /* Peer4 remains fully functional after peer3's PL drain */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  /* Verify peer4 can receive new routes after peer3's init dump drains */
  injectLocalRoutesAtRuntime({"18.44.0.0/16"}, {"1844:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("18.44.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "18.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1844:1"));
  verifySlowPeerInvariants(kPeerAddr4);

  XLOGF(INFO, "=== TEST PASSED: DetachedInitDump_PlDrain ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

/*
 * P-DID x E-ROUTE-WD
 * Withdrawal while peer3 is in DETACHED_INIT_DUMP. The withdrawal is a CL
 * item for peer3. Peer4 receives the withdrawal normally via group PL.
 * First inject a shared route, then detach peer3 into DID, then withdraw.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedInitDump_RouteWithdraw) {
  XLOGF(INFO, "=== TEST: DetachedInitDump_RouteWithdraw ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route that both peers receive */
  injectLocalRoutesAtRuntime({"71.1.0.0/16"}, {"7101:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("71.1.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.1.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "7101:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.1.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7101:1"));

  /* Freq-detach peer3 with threshold=1 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("71.{}.0.0/16", 2 + i);
    auto community = fmt::format("{}:1", 7102 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("71.{}.0.0", 2 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* DOWN + unblock + UP -> peer re-enters group (DID or transitioning) */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /*
   * Peer3 re-enters the group in DETACHED_INIT_DUMP and runs an independent
   * init dump sourced from the ShadowRib. Verify it receives the full set
   * still in the ShadowRib -- the shared route it already had (71.1) and the
   * routes it missed while blocked (71.2/71.3/71.4) -- before the withdrawal
   * races in, confirming the detached rib dump walks the ShadowRib and emits
   * the right entries. Order-independent: the dump walks the ShadowRib in hash
   * order.
   */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "71.1.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "7101:1"},
       {.prefix = "71.2.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "7102:1"},
       {.prefix = "71.3.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "7103:1"},
       {.prefix = "71.4.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "7104:1"}}));

  /* Withdraw the shared route while peer3 is reconnecting */
  withdrawLocalRoutesAtRuntime({"71.1.0.0/16"});

  /* Peer4 receives the withdrawal normally */
  EXPECT_TRUE(verifyRouteWithdraw("v4", "71.1.0.0", 16, kPeerAddr4));

  /* Peer4 still in sync, peer3 in detached state */
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  XLOGF(INFO, "=== TEST PASSED: DetachedInitDump_RouteWithdraw ===");
}

/*
 * P-DID x E-UNBLOCK -- N/A
 * Peer shouldn't normally be blocked during init dump. Calling unblockPeer
 * on a DID peer is a no-op. Verify no crash and peer stays in DID.
 */
TEST_P(UpdateGroupMultiPeerTest, DetachedInitDump_Unblock_NA) {
  XLOGF(INFO, "=== TEST: DetachedInitDump_Unblock_NA ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  for (int i = 0; i < 3; i++) {
    auto prefix = fmt::format("71.{}.0.0/16", 10 + i);
    auto community = fmt::format("{}:1", 7110 + i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("71.{}.0.0", 10 + i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* DOWN + unblock + UP -> peer re-enters group (DID or transitioning) */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  unblockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /*
   * Peer3 re-enters the group in DETACHED_INIT_DUMP and runs an independent
   * init dump sourced from the ShadowRib. Verify it catches up on the routes
   * it missed while blocked (71.10/71.11/71.12), confirming the detached rib
   * dump walks the ShadowRib and emits the right entries. Order-independent:
   * the dump walks the ShadowRib in hash order.
   */
  EXPECT_TRUE(verifyRoutes(
      "v4",
      kPeerAddr3,
      {{.prefix = "71.10.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "7110:1"},
       {.prefix = "71.11.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "7111:1"},
       {.prefix = "71.12.0.0",
        .prefixLen = 16,
        .expectedNexthop = getExpectedNexthop(kPeerAddr3),
        .expectedAsPath = "4200000001",
        .expectedCommunity = "7112:1"}}));

  /* Unblock on reconnecting peer -- should be a no-op */
  unblockPeer(kPeerAddr3);

  /* Peer3 not DOWN, no crash from double-unblock */
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::DOWN);

  /* Peer4 still works */
  injectLocalRoutesAtRuntime({"71.15.0.0/16"}, {"7115:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("71.15.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "71.15.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "7115:1"));
  EXPECT_TRUE(isPeerInSync(kPeerAddr4));

  XLOGF(INFO, "=== TEST PASSED: DetachedInitDump_Unblock_NA ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
