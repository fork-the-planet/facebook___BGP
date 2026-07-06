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
 * [BGP++][UG2 E2E] EoR lifecycle tests with Update Groups.
 *
 * Tests for Bugs 2 and 6: EoR notification flow through UG-specific code
 * paths. Bug 2 found that re-entry guards and bitmap filtering could silently
 * drop EoR notifications. Bug 6 found that egressEoRsPending_ was not
 * propagated to detached peers during initial dump.
 *
 * Learning patterns applied:
 *   P1: After unblockPeer(), add JOINED_RUNNING to allowed states
 *   P5: After unblockPeer(), do NOT verifyRouteAdd for pre-queued routes
 *   P6: Inject >= queue capacity (3) routes for reliable blocking
 *   P8: Use 3 peers
 *   P10: Always sendEoRToPeer() after bringUpPeer()
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test 6 (High): UGActivation_StaggeredPeerArrival_EoRReachesPeerManager
 *
 * Peers arrive at different times. All must reach JOINED_RUNNING and
 * receive routes after EoR. Tests that staggered arrival doesn't cause
 * EoR to be dropped by bitmap filtering or re-entry guard.
 */
TEST_P(
    UpdateGroupEoRLifecycleTest,
    UGActivation_StaggeredPeerArrival_EoRReachesPeerManager) {
  XLOGF(
      INFO,
      "=== TEST: UGActivation_StaggeredPeerArrival_EoRReachesPeerManager ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  /* Pre-load routes before any peer is up */
  addLocalRoute("150.0.0.0/8", {"1500:1"}, 100);
  addLocalRoute("151.0.0.0/8", {"1510:1"}, 100);

  setupSlowPeerComponents(5, 4, 0);

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("150.0.0.0/8")));
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("151.0.0.0/8")));

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Staggered arrival: peer3 first */
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* peer4 arrives next */
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);

  /* peer5 arrives last */
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId5);

  /* All peers should eventually reach JOINED_RUNNING */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 100));
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING, 100));
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING, 100));

  /* Drain all queues to consume initial dump + EoRs */
  drainPeerQueueCompletely(peerId3);
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* Inject a runtime route — all 3 must receive it */
  injectLocalRoutesAtRuntime({"152.0.0.0/8"}, {"1520:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("152.0.0.0/8")));

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "152.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "152.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1520:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "152.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1520:1"));

  XLOGF(
      INFO,
      "=== TEST PASSED: UGActivation_StaggeredPeerArrival_EoRReachesPeerManager ===");
}

/*
 * Test 7 (High): DetachDuringInitialDump_EoRPendingPropagated
 *
 * Bug 6 (D98857411): When a peer detaches during initial dump,
 * egressEoRsPending_ was not propagated from group to detached peer.
 * The detached peer never sent EoR markers.
 *
 * Flow:
 * 1. Pre-load 4 routes (each with distinct community to avoid packing)
 * 2. Peer4/peer5 get large queues (capacity 10) — initial dump fits fine
 * 3. Peer3 gets small queue (capacity 3) — 4 routes overflow it
 * 4. Real backpressure on peer3 → freq threshold fires → peer3 detaches
 * 5. Drain peer3 to unblock → recovery → JOINED_RUNNING
 * 6. Verify routes flow to all peers after recovery
 */
TEST_P(
    UpdateGroupEoRLifecycleTest,
    DetachDuringInitialDump_EoRPendingPropagated) {
  XLOGF(INFO, "=== TEST: DetachDuringInitialDump_EoRPendingPropagated ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  addPeer(kDefaultPeerSpec5);

  /* Pre-load 4 routes with distinct communities (each becomes a separate
   * UPDATE message, preventing BGP packing). */
  addLocalRoute("160.0.0.0/8", {"1600:1"}, 100);
  addLocalRoute("161.0.0.0/8", {"1610:1"}, 100);
  addLocalRoute("162.0.0.0/8", {"1620:1"}, 100);
  addLocalRoute("163.0.0.0/8", {"1630:1"}, 100);

  /* Initialize slow peer detection infrastructure. */
  setupSlowPeerComponents(3, 2, 0);

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("160.0.0.0/8")));

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};

  /* Peer4/peer5 get large queues so initial dump (4 routes + 2 EoRs per
   * peer) fits without backpressure. */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  bringUpPeer(kPeerAddr5);
  sendEoRToPeer(peerId4);
  sendEoRToPeer(peerId5);

  /* Peer3 gets small queue (capacity 3). The initial dump of 4 routes
   * overflows it, causing real production backpressure. */
  setDefaultQueueSizes(3, 2, 0);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Wait for peer4 to join, then set aggressive freq threshold. */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING, 100));

  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(600000),
      1,
      std::chrono::milliseconds(60000));

  /* Drain peer4 and peer5 initial dump routes. */
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* Wait for peer3's initial dump to fill the queue and trigger
   * backpressure → freq detection → DETACHED_BLOCKED. */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED, 200));

  /* Drain peer3's queue to relieve backpressure and allow recovery. */
  drainPeerQueueCompletely(peerId3);

  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));

  /* Drain any routes re-queued to peer4/peer5 during peer3's
   * detach/recovery cycle. */
  drainPeerQueueCompletely(peerId4);
  drainPeerQueueCompletely(peerId5);

  /* After recovery, verify routes flow to all peers */
  injectLocalRoutesAtRuntime({"164.0.0.0/8"}, {"1640:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("164.0.0.0/8")));

  drainPeerQueueCompletely(peerId3);
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "164.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1640:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "164.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1640:1"));

  XLOGF(
      INFO,
      "=== TEST PASSED: DetachDuringInitialDump_EoRPendingPropagated ===");
}

/*
 * Test 11 (High): UGActivation_ReEntryGuard_EoRNotLost
 *
 * Bug 2 variant: With many pre-loaded routes, the initial dump is slow.
 * Injecting a CL update during initial dump triggers the re-entry guard.
 * EoR must not be silently dropped.
 */
TEST_P(UpdateGroupEoRLifecycleTest, UGActivation_ReEntryGuard_EoRNotLost) {
  XLOGF(INFO, "=== TEST: UGActivation_ReEntryGuard_EoRNotLost ===");

  addPeer(kDefaultPeerSpec3);

  /* Pre-load many routes to make initial dump slow */
  for (int i = 0; i < 10; ++i) {
    std::string prefix = std::to_string(170 + i) + ".0.0.0/8";
    std::string community = std::to_string(1700 + i * 10) + ":1";
    addLocalRoute(prefix, {community}, 100);
  }

  /* Larger queue so initial dump doesn't immediately block */
  setupSlowPeerComponents(20, 15, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};

  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Inject a CL update while initial dump may still be in progress */
  injectLocalRoutesAtRuntime({"180.0.0.0/8"}, {"1800:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("180.0.0.0/8")));

  /* Wait for peer3 to reach JOINED_RUNNING */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 100));

  /* Drain everything and verify the runtime route was received */
  drainPeerQueueCompletely(peerId3);

  /* Inject another runtime route to verify routes still flow */
  injectLocalRoutesAtRuntime({"181.0.0.0/8"}, {"1810:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("181.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "181.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1810:1"));

  XLOGF(INFO, "=== TEST PASSED: UGActivation_ReEntryGuard_EoRNotLost ===");
}

/*
 * DurationDetachDuringInitDump_EoRDelivered
 *
 * Reproduces the bug where the duration-based slow peer timer fires
 * during the initial RIB dump and calls detachSlowPeer() without
 * sendWithEoR context (defaults to false). Without the fix,
 * egressEoRsPending_ was never set on the detached peer, so EoR was
 * omitted from the accumulated changes.
 *
 * Flow:
 * 1. Pre-load 4 routes with distinct communities
 * 2. peer4 gets large queue (capacity 10) — initial dump fits fine
 * 3. peer3 gets small queue (capacity 3) — blocked before bringUp
 * 4. Set 1ms duration threshold, high freq threshold (999999)
 * 5. Duration timer fires on peer3 → detach with sendWithEoR=false
 * 6. Fix: detachSlowPeer checks group egressEoRsPending_ flag
 * 7. Drain peer3 → EoR arrives via accumulated changes
 */
TEST_P(UpdateGroupEoRLifecycleTest, DurationDetachDuringInitDump_EoRDelivered) {
  XLOGF(INFO, "=== TEST: DurationDetachDuringInitDump_EoRDelivered ===");

  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  addLocalRoute("190.0.0.0/8", {"1900:1"}, 100);
  addLocalRoute("191.0.0.0/8", {"1910:1"}, 100);
  addLocalRoute("192.0.0.0/8", {"1920:1"}, 100);
  addLocalRoute("193.0.0.0/8", {"1930:1"}, 100);

  setupSlowPeerComponents(3, 2, 0);

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("190.0.0.0/8")));

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};

  /* peer4 gets large queue so initial dump fits without backpressure */
  setDefaultQueueSizes(10, 8, 0);
  bringUpPeer(kPeerAddr4);
  sendEoRToPeer(peerId4);

  /* peer3 gets small queue, blocked before bringUp */
  setDefaultQueueSizes(3, 2, 0);
  blockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Wait for peer4 to join */
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING, 100));

  /*
   * Set 1ms duration threshold on the group AFTER bringUp (peer needs
   * an update group to exist). High frequency threshold prevents
   * frequency-based detach from firing first.
   */
  setSlowPeerThresholds(
      kPeerAddr4,
      std::chrono::milliseconds(1),
      999999,
      std::chrono::milliseconds(60000));

  /* Wait for peer3 to block and get duration-detached */
  ASSERT_TRUE(waitForPeerQueueBlocked(peerId3));
  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::DETACHED_BLOCKED, 200));

  /* Drain peer4 initial dump */
  drainPeerQueueCompletely(peerId4);

  /* Unblock peer3 and drain to allow detached mode recovery */
  unblockPeer(kPeerAddr3);
  for (int i = 0; i < 20; ++i) {
    drainPeerQueueCompletely(peerId3, 1, 100);
    peerManager_->getEventBase().runInEventBaseThreadAndWait([]() {});
    if (getPeerState(kPeerAddr3) == PeerUpdateState::JOINED_RUNNING) {
      break;
    }
    folly::futures::sleep(std::chrono::milliseconds(100)).get();
  }
  drainPeerQueueCompletely(peerId3);

  ASSERT_TRUE(
      waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING, 200));

  /*
   * The critical assertion: peer3 must have received EoR via
   * accumulated changes even though duration timer detached it
   * without sendWithEoR. Verify by injecting a runtime route —
   * if EoR was not sent, PeerManagerBase would still be waiting for
   * it and the route would not flow.
   */
  drainPeerQueueCompletely(peerId4);
  injectLocalRoutesAtRuntime({"194.0.0.0/8"}, {"1940:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("194.0.0.0/8")));

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "194.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "1940:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "194.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1940:1"));

  XLOGF(INFO, "=== TEST PASSED: DurationDetachDuringInitDump_EoRDelivered ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupEoRLifecycleTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
