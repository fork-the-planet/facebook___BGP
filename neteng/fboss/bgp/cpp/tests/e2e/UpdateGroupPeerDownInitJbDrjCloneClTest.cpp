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
 * E2E tests for peer DOWN from various states:
 * DOWN from P-INIT (abort init dump)
 * DOWN from P-JB (cancel duration timer, adjust bitmaps)
 * DOWN from P-DRJ (discard ready state, full cleanup)
 * DOWN with accumulated lazy clones (per-peer entries removed)
 * DOWN during CL consumption (coroutine cancelled cleanly)
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Peer DOWN from P-INIT — abort init dump, clean state.
 * Bring up peer with pre-loaded routes (single peer, Pattern 7),
 * then immediately bring it down before EoR. Verify clean cleanup.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownFromInit) {
  XLOG(INFO, "=== TEST: PeerDownFromInit ===");

  /*
   * This test brings the peer down before it ever sends an EoR, so peer
   * manager initialization can only complete when the EoR convergence timer
   * expires. Shorten that timer (default 45s) so the peer reaches DOWN
   * promptly; the test asserts only the DOWN transition and clean shutdown,
   * with no timing dependency, so the outcome is identical. Must be set before
   * getConfig() runs (i.e. before setupSlowPeerComponents creates the peer
   * manager).
   */
  setEorTimeSeconds(2);

  addPeer(kDefaultPeerSpec3);
  addLocalRoute("30.1.0.0/16", {"3001:1"}, 100);

  setupSlowPeerComponents(3, 2, 0);

  auto routePrefix = folly::IPAddress::createNetwork("30.1.0.0/16");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix));

  /* Bring up peer — enters INIT with init dump in progress */
  bringUpPeer(kPeerAddr3);

  /* Immediately bring it down before EoR — aborts init dump */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* No crashes — clean shutdown verified by TearDown */
  XLOG(INFO, "=== TEST PASSED: PeerDownFromInit ===");
}

/*
 * Peer DOWN from P-JB — cancel duration timer, adjust bitmaps.
 * Set a long duration timer, block peer3 until JOINED_BLOCKED,
 * then bring it DOWN. Verify timer is cancelled and bitmaps adjusted.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownFromJoinedBlockedWithTimer) {
  XLOG(INFO, "=== TEST: PeerDownFromJoinedBlockedWithTimer ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Set a long duration threshold — timer starts when blocked */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      100,
      std::chrono::milliseconds(60000));

  /* Block peer3 and fill queue to trigger JOINED_BLOCKED */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.10.0.0/16"}, {"3010:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.10.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.10.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3010:1"));
  injectLocalRoutesAtRuntime({"30.11.0.0/16"}, {"3011:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.11.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.11.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3011:1"));
  injectLocalRoutesAtRuntime({"30.12.0.0/16"}, {"3012:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.12.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.12.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3012:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  auto stateBlocked = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      stateBlocked == PeerUpdateState::JOINED_BLOCKED ||
      stateBlocked == PeerUpdateState::DETACHED_BLOCKED);

  /* Bring DOWN peer3 while JOINED_BLOCKED with active duration timer */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 should still be functional — timer was cancelled, no crash */
  injectLocalRoutesAtRuntime({"30.13.0.0/16"}, {"3013:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.13.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.13.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3013:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerDownFromJoinedBlockedWithTimer ===");
}

/*
 * Peer DOWN from P-DRJ — discard ready state, full cleanup.
 * Reach DRJ via freq-detach then unblock, then bring DOWN.
 * Verify detached peer count goes to 0 and peer4 continues.
 */
TEST_P(UpdateGroupPeerDownTest, PeerDownFromDetachedReadyToJoin) {
  XLOG(INFO, "=== TEST: PeerDownFromDetachedReadyToJoin ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Freq-detach peer3: threshold=1 block triggers detachment */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.20.0.0/16"}, {"3020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.20.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.20.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3020:1"));
  injectLocalRoutesAtRuntime({"30.21.0.0/16"}, {"3021:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.21.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.21.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3021:1"));
  injectLocalRoutesAtRuntime({"30.22.0.0/16"}, {"3022:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.22.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.22.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3022:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Unblock → may transition to DRJ, DB, JR, or JB depending on CL batch */
  unblockPeer(kPeerAddr3);
  auto state = getPeerState(kPeerAddr3);
  EXPECT_NE(state, PeerUpdateState::INIT);

  /* Bring DOWN from current state */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Peer4 still functional */
  injectLocalRoutesAtRuntime({"30.23.0.0/16"}, {"3023:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.23.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.23.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3023:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerDownFromDetachedReadyToJoin ===");
}

/*
 * Peer DOWN with accumulated lazy clones — all per-peer entries
 * removed. Detach peer3, inject several routes post-detach (creating CL entries
 * with lazy clone per-peer entries), then bring peer3 DOWN. Verify cleanup.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerDownWithAccumulatedClones) {
  XLOG(INFO, "=== TEST: PeerDownWithAccumulatedClones ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Inject a shared route BEFORE detachment (both peers receive it) */
  injectLocalRoutesAtRuntime({"30.30.0.0/16"}, {"3030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.30.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "3030:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.30.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3030:1"));

  /* Freq-detach peer3 */
  setSlowPeerThresholds(
      kPeerAddr3,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.31.0.0/16"}, {"3031:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.31.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.31.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3031:1"));
  injectLocalRoutesAtRuntime({"30.32.0.0/16"}, {"3032:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.32.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.32.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3032:1"));
  injectLocalRoutesAtRuntime({"30.33.0.0/16"}, {"3033:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.33.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.33.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3033:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Inject several more routes post-detach — creates CL entries with
   * lazy clone per-peer entries (Case 4 clones for the shared route,
   * Case 2 no-clone for new prefixes). Peer4 receives them all. */
  for (int i = 34; i <= 38; i++) {
    auto prefix = fmt::format("30.{}.0.0/16", i);
    auto community = fmt::format("30{}:1", i);
    injectLocalRoutesAtRuntime({prefix}, {community}, 150);
    ASSERT_TRUE(
        waitForRouteInShadowRib(folly::IPAddress::createNetwork(prefix)));
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        fmt::format("30.{}.0.0", i),
        16,
        kPeerAddr4,
        getExpectedNexthop(kPeerAddr4),
        "4200000001",
        community));
  }

  /* Peer3 is DETACHED_BLOCKED with accumulated CL entries and clones */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED));

  /* Bring DOWN peer3 — all per-peer entries should be cleaned up */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  EXPECT_EQ(getDetachedPeerCount(kPeerAddr4), 0);

  /* Peer4 still functional after clone cleanup */
  injectLocalRoutesAtRuntime({"30.39.0.0/16"}, {"3039:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.39.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.39.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3039:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerDownWithAccumulatedClones ===");
}

/*
 * Peer DOWN during CL consumption — coroutine cancelled cleanly.
 * Inject routes while group is WAITING (CL items accumulate), then
 * bring peer3 DOWN while CL items are pending. Verify no crash.
 */
TEST_P(UpdateGroupMultiPeerTest, PeerDownDuringClConsumption) {
  XLOG(INFO, "=== TEST: PeerDownDuringClConsumption ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Consume v4 + v6 EoRs (dual-stack sends 2 EoRs per peer) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));
  EXPECT_TRUE(waitForEoR(peerId4));

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));

  /* Block peer3 to create WAITING state */
  blockPeer(kPeerAddr3);
  injectLocalRoutesAtRuntime({"30.40.0.0/16"}, {"3040:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.40.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.40.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3040:1"));
  injectLocalRoutesAtRuntime({"30.41.0.0/16"}, {"3041:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.41.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.41.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3041:1"));
  injectLocalRoutesAtRuntime({"30.42.0.0/16"}, {"3042:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.42.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.42.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3042:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));
  auto stateBlocked = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      stateBlocked == PeerUpdateState::JOINED_BLOCKED ||
      stateBlocked == PeerUpdateState::DETACHED_BLOCKED);

  /* Inject more routes while WAITING — these become CL items */
  injectLocalRoutesAtRuntime({"30.43.0.0/16"}, {"3043:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.43.0.0/16")));
  injectLocalRoutesAtRuntime({"30.44.0.0/16"}, {"3044:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.44.0.0/16")));

  /* Bring peer3 DOWN while CL items are pending */
  bringDownPeer(kPeerAddr3);
  EXPECT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  /* Unblock peer3's test-level block (cleanup) */
  unblockPeer(kPeerAddr3);

  /* Consume CL items (30.43, 30.44) that were pending before peer3 DOWN */
  verifyRouteAdd(
      "v4",
      "30.43.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3043:1");
  verifyRouteAdd(
      "v4",
      "30.44.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3044:1");

  /* Peer4 should still be functional — CL coroutine cancelled cleanly */
  injectLocalRoutesAtRuntime({"30.45.0.0/16"}, {"3045:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("30.45.0.0/16")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.45.0.0",
      16,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "3045:1"));

  verifySlowPeerInvariants(kPeerAddr4);

  XLOG(INFO, "=== TEST PASSED: PeerDownDuringClConsumption ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupPeerDownTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupMultiPeerTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
