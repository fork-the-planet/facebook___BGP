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
 * E2E tests for BGP Update Group MVP functionality - Peer Lifecycle scenarios
 * Tests complete flow: RIB → PeerManagerBase → UpdateGroup → Peers
 * Requires: Change List Tracker + Update Group + Egress Backpressure
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test 8: Bring peer down and up, verify routes relearned
 */
TEST_P(UpdateGroupPeerLifecycleTest, PeerDownUpRelearn) {
  /* Add peers to configuration */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  addLocalRoute("28.0.0.0/8", {"308:1"}, 100);

  XLOG(INFO, "=== TEST: Peer down/up relearn ===");

  setupComponents();

  /* Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("28.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 28.0.0.0/8 did not reach shadowRIB in time";

  /* Bring up peers */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to each peer (ingress EoR) */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Consume route UPDATEs from BOTH peers */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "28.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "308:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "28.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "308:1"));

  /* Wait for egress EoR from all peers (final PDU) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Bring peer3 down */
  XLOG(INFO, "Bringing peer3 down");
  bringDownPeer(kPeerAddr3);

  /* Bring peer3 back up */
  XLOG(INFO, "Bringing peer3 back up");
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Peer3 should relearn the route */
  XLOGF(
      INFO,
      "Verifying peer3 relearns route 28.0.0.0/8 with nexthop {}",
      getExpectedNexthop(kPeerAddr3));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "28.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "308:1"));

  /* Wait for peer3 EoR after reconnect */
  EXPECT_TRUE(waitForEoR(peerId3));

  /*
   * NOTE: After reconnect, peer3 is in DETACHED state and does NOT rejoin
   * the update group for runtime routes (rejoin code not implemented yet).
   * Only peer4 will receive runtime route updates.
   */

  /* Phase 2: Add more runtime routes */
  XLOG(INFO, "Phase 2: Adding more runtime routes");
  injectLocalRoutesAtRuntime({"30.0.0.0/8"}, {"310:1"}, 150);
  injectLocalRoutesAtRuntime({"31.0.0.0/8"}, {"311:1"}, 150);

  /* Only peer4 (not reconnected) receives runtime routes */
  std::vector<VerifySpec> expectedRuntimeRoutes = {
      {.prefix = "30.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "310:1"},
      {.prefix = "31.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "311:1"}};
  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr4, expectedRuntimeRoutes));

  /* Phase 3: WITHDRAW routes */
  XLOG(INFO, "Phase 3: Withdrawing routes");
  withdrawLocalRoutesAtRuntime({"30.0.0.0/8"});

  EXPECT_TRUE(verifyRouteWithdraw("v4", "30.0.0.0", 8, kPeerAddr4));

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding withdrawn route");
  injectLocalRoutesAtRuntime({"30.0.0.0/8"}, {"310:2"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "30.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "310:2"));

  /* Phase 5: CHANGE attributes */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"31.0.0.0/8"}, {"311:99"}, 150);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "31.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "311:99"));

  XLOG(INFO, "=== TEST PASSED: Peer relearned routes ===");
}

/*
 * Test 9: Last peer of group down, first peer up
 */
TEST_P(UpdateGroupPeerLifecycleTest, LastPeerDownFirstPeerUp) {
  /* Add peers to configuration */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  addLocalRoute("29.0.0.0/8", {"309:1"}, 100);

  XLOG(INFO, "=== TEST: Last peer down, first peer up ===");

  setupComponents();

  /* Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("29.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 29.0.0.0/8 did not reach shadowRIB in time";

  /* Bring up both peers */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to each peer (ingress EoR) */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Consume route UPDATEs */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "309:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "309:1"));

  /* Wait for egress EoR from all peers (final PDU) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Bring down peer4 (last peer) */
  XLOG(INFO, "Bringing down peer4 (last peer)");
  bringDownPeer(kPeerAddr4);

  /* Bring down peer3 (first peer) */
  XLOG(INFO, "Bringing down peer3 (first peer)");
  bringDownPeer(kPeerAddr3);

  /* Bring peer3 back up */
  XLOG(INFO, "Bringing peer3 back up");
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Peer3 should get the route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "29.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "309:1"));

  /* Wait for peer3 EoR after reconnect */
  EXPECT_TRUE(waitForEoR(peerId3));

  /*
   * NOTE: After reconnect, peer3 is in DETACHED state and does NOT rejoin
   * the update group for runtime routes (rejoin code not implemented yet).
   * Since peer3 is the only peer up, we skip runtime route testing here
   * as the detached peer won't receive them from the change list.
   */

  XLOG(INFO, "=== TEST PASSED: Last peer down, first up ===");
}

/*
 * Simple peer reconnect test
 * - Bring peer up, send route, bring down, bring back up
 * - This tests the update group's ability to re-announce routes after peer
 *   reconnect
 */
TEST_P(UpdateGroupPeerLifecycleTest, SimplePeerReconnect) {
  XLOG(INFO, "=== TEST START: SimplePeerReconnect ===");

  /* Add peer config */
  addPeer(kDefaultPeerSpec3);

  /* Add local route to config (before RIB creation) */
  XLOG(INFO, "Adding local route 50.0.0.0/8 to config");
  addLocalRoute("50.0.0.0/8", {"500:1"}, 100);

  /* Setup - RIB will announce the local route */
  XLOG(INFO, "Creating RIB and PeerManagerBase");
  setupComponents();

  /* Wait for route to reach shadowRIB */
  XLOG(INFO, "Waiting for route to reach shadowRIB");
  auto routePrefix = folly::IPAddress::createNetwork("50.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 50.0.0.0/8 did not reach shadowRIB in time";

  /* Bring peer3 up */
  XLOG(INFO, "Bringing peer3 up for the first time");
  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  /* Peer3 should get the route during initial dump */
  XLOG(INFO, "Expecting peer3 to receive route during initial dump");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "500:1"));

  /* Wait for EoR */
  XLOG(INFO, "Waiting for peer3 initial EoR");
  EXPECT_TRUE(waitForEoR(peerId3));

  /* Bring peer3 down */
  XLOG(INFO, "Bringing peer3 down");
  bringDownPeer(kPeerAddr3);

  /* Bring peer3 back up */
  XLOG(INFO, "Bringing peer3 back up (reconnect)");
  bringUpPeer(kPeerAddr3);
  sendEoRToPeer(peerId3);

  /* Peer3 should relearn the local route (still in shadowRIB) */
  XLOG(INFO, "Expecting peer3 to relearn route after reconnect");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "50.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "500:1"));

  /* Wait for peer3 EoR after reconnect */
  XLOG(INFO, "Waiting for peer3 EoR after reconnect");
  EXPECT_TRUE(waitForEoR(peerId3));

  /*
   * NOTE: After reconnect, peer3 is in DETACHED state and does NOT rejoin
   * the update group for runtime routes (rejoin code not implemented yet).
   * Since peer3 is the only peer up, we skip runtime route testing here
   * as the detached peer won't receive them from the change list.
   */

  /* Explicitly bring down peer to ensure clean teardown */
  XLOG(INFO, "Explicitly bringing down peer3");
  bringDownPeer(kPeerAddr3);

  XLOG(INFO, "=== TEST PASSED: SimplePeerReconnect ===");
}

/*
 * Instantiate tests for both serialization modes.
 */
INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupPeerLifecycleTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
