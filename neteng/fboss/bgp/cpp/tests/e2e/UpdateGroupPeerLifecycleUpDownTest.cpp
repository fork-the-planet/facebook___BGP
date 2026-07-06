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
 * Test 5: Bring peers down and up, verify routes still work
 */
TEST_P(UpdateGroupPeerLifecycleTest, PeersDownAndUp) {
  XLOG(INFO, "=== TEST: Peers down and up ===");

  /* Step 1: Add peers to configuration */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  BgpPeerSpec spec5 = {
      kPeerAsn5, kLocalAddr1, kPeerAddr5, kNextHopV4_5, kNextHopV6_5};
  addPeer(spec5);

  /* Step 2: Add local route to config */
  addLocalRoute("24.0.0.0/8", {"304:1"}, 100);

  /* Step 3: Create RIB and PeerManagerBase */
  setupComponents();

  /* Step 4: Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("24.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 24.0.0.0/8 did not reach shadowRIB in time";

  std::vector<folly::IPAddress> peers = {kPeerAddr3, kPeerAddr4, kPeerAddr5};

  /* Step 5: Bring up peers */
  for (const auto& peer : peers) {
    bringUpPeer(peer);
  }

  /* Step 5: Send EoR to each peer (ingress EoR) */
  for (const auto& peer : peers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    sendEoRToPeer(peerId);
  }

  /* Step 6: Verify route on all peers */
  for (const auto& peer : peers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "24.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "304:1"));
  }

  /* Step 7: Wait for EoR from all peers */
  for (const auto& peer : peers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Bring down peer3 */
  XLOG(INFO, "Bringing down peer3");
  bringDownPeer(kPeerAddr3);

  /* Bring peer3 back up */
  XLOG(INFO, "Bringing peer3 back up");
  bringUpPeer(kPeerAddr3);
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  /* Verify peer3 gets the route again */
  XLOGF(
      INFO,
      "Verifying peer3 gets route 24.0.0.0/8 again with nexthop {}",
      getExpectedNexthop(kPeerAddr3));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "24.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "304:1"));

  /* Wait for peer3 EoR after reconnect */
  EXPECT_TRUE(waitForEoR(peerId3));

  /*
   * NOTE: After reconnect, peer3 is in DETACHED state and does NOT rejoin
   * the update group for runtime routes (rejoin code not implemented yet).
   * Only peer4 and peer5 will receive runtime route updates.
   */
  std::vector<folly::IPAddress> activePeers = {kPeerAddr4, kPeerAddr5};

  /* Phase 2: Add more routes at runtime */
  XLOG(INFO, "Phase 2: Adding more routes at runtime");
  injectLocalRoutesAtRuntime({"25.0.0.0/8"}, {"304:2"}, 100);
  injectLocalRoutesAtRuntime({"26.0.0.0/8"}, {"304:3"}, 100);

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("25.0.0.0/8")));
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("26.0.0.0/8")));

  /* Verify runtime routes on active peers only (peer3 is detached) */
  for (const auto& peer : activePeers) {
    std::vector<VerifySpec> expectedRoutes = {
        {.prefix = "25.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "304:2"},
        {.prefix = "26.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "304:3"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedRoutes));
  }

  /* Phase 3: WITHDRAW one route */
  XLOG(INFO, "Phase 3: Withdrawing route 25");
  withdrawLocalRoutesAtRuntime({"25.0.0.0/8"});

  std::vector<WithdrawSpec> expectedWithdraws = {
      {.prefix = "25.0.0.0", .prefixLen = 8}};

  for (const auto& peer : activePeers) {
    EXPECT_TRUE(verifyRouteWithdraws("v4", peer, expectedWithdraws));
  }

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding route 25");
  injectLocalRoutesAtRuntime({"25.0.0.0/8"}, {"304:2"}, 100);

  /* Verify re-added route on active peers only */
  for (const auto& peer : activePeers) {
    std::vector<VerifySpec> expectedReAdded = {
        {.prefix = "25.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "304:2"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedReAdded));
  }

  /* Phase 5: CHANGE attributes */
  XLOG(INFO, "Phase 5: Changing attributes for route 26");
  injectLocalRoutesAtRuntime({"26.0.0.0/8"}, {"304:99"}, 200);

  /* Verify changed route on active peers only */
  for (const auto& peer : activePeers) {
    std::vector<VerifySpec> expectedChanged = {
        {.prefix = "26.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "304:99"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedChanged));
  }

  XLOG(INFO, "=== TEST PASSED: Peers down/up ===");
}

/*
 * Test 6: After BGP initializes, bring up multiple peers
 * Peers should form update group and receive routes
 */
TEST_P(UpdateGroupPeerLifecycleTest, BringPeersAfterInit) {
  XLOG(INFO, "=== TEST: Bring peers after init ===");

  /* Step 1: Add peer configs but don't bring them up yet */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  BgpPeerSpec spec5 = {
      kPeerAsn5, kLocalAddr1, kPeerAddr5, kNextHopV4_5, kNextHopV6_5};
  addPeer(spec5);

  /* Step 2: Add local route to config */
  addLocalRoute("25.0.0.0/8", {"305:1"}, 100);

  /* Step 3: Create RIB and PeerManagerBase */
  setupComponents();

  /* Step 4: Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("25.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 25.0.0.0/8 did not reach shadowRIB in time";

  /* Step 5: NOW bring up peers */
  XLOG(INFO, "Bringing up peers after BGP init");
  std::vector<folly::IPAddress> peers = {kPeerAddr3, kPeerAddr4, kPeerAddr5};
  for (const auto& peer : peers) {
    bringUpPeer(peer);
  }

  /* Step 5: Send EoR to each peer (ingress EoR) */
  for (const auto& peer : peers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    sendEoRToPeer(peerId);
  }

  /* Step 6: Peers should receive the local route via update group */
  for (const auto& peer : peers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "25.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "305:1"));
  }

  /* Step 7: Wait for EoR from all peers (final PDU) */
  for (const auto& peer : peers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Phase 2: Add more runtime routes */
  XLOG(INFO, "Phase 2: Adding more runtime routes");
  injectLocalRoutesAtRuntime({"36.0.0.0/8"}, {"316:1"}, 150);
  injectLocalRoutesAtRuntime({"37.0.0.0/8"}, {"317:1"}, 150);

  for (const auto& peer : peers) {
    std::vector<VerifySpec> expectedRuntimeRoutes = {
        {.prefix = "36.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "316:1"},
        {.prefix = "37.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "317:1"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedRuntimeRoutes));
  }

  /* Phase 3: WITHDRAW routes */
  XLOG(INFO, "Phase 3: Withdrawing routes");
  withdrawLocalRoutesAtRuntime({"36.0.0.0/8"});

  for (const auto& peer : peers) {
    EXPECT_TRUE(verifyRouteWithdraw("v4", "36.0.0.0", 8, peer));
  }

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding withdrawn route");
  injectLocalRoutesAtRuntime({"36.0.0.0/8"}, {"316:2"}, 150);

  for (const auto& peer : peers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "36.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "316:2"));
  }

  /* Phase 5: CHANGE attributes */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"37.0.0.0/8"}, {"317:99"}, 150);

  for (const auto& peer : peers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "37.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "317:99"));
  }

  XLOG(INFO, "=== TEST PASSED: Peers after init ===");
}

/*
 * Test 7: Add route, then add new peer, verify JOIN works
 * New peer should get initial dump + future updates
 */
TEST_P(UpdateGroupPeerLifecycleTest, NewPeerJoin) {
  XLOG(INFO, "=== TEST: New peer JOIN ===");

  /* Step 1: Add peers to configuration */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  BgpPeerSpec spec5 = {
      kPeerAsn5, kLocalAddr1, kPeerAddr5, kNextHopV4_5, kNextHopV6_5};
  addPeer(spec5);

  /* Step 2: Add local route to config */
  addLocalRoute("26.0.0.0/8", {"306:1"}, 100);

  /* Step 3: Setup components */
  setupComponents();

  /* Step 4: Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("26.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 26.0.0.0/8 did not reach shadowRIB in time";

  /* Bring up only first 2 peers initially */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to each peer (ingress EoR) */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Consume route UPDATEs for first 2 peers */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "306:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "306:1"));

  /* Wait for egress EoR from all peers (final PDU) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* NOW bring up peer5 - simulates JOIN */
  XLOG(INFO, "Bringing up peer5 - simulating JOIN");
  bringUpPeer(kPeerAddr5);
  BgpPeerId peerId5{kPeerAddr5, kPeerAddr5.asV4().toLongHBO()};
  sendEoRToPeer(peerId5);

  /* Peer5 should get initial dump of existing route */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "26.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "306:1"));

  /* Wait for peer5 EoR */
  EXPECT_TRUE(waitForEoR(peerId5));

  /* Inject new route after peer5 joined */
  XLOG(INFO, "Injecting new route after peer5 joined");
  injectLocalRoutesAtRuntime({"27.0.0.0/8"}, {"307:1"}, 150);

  /* All 3 peers should get the new route (JOIN worked!) */
  std::vector<folly::IPAddress> allPeers = {kPeerAddr3, kPeerAddr4, kPeerAddr5};
  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "27.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "307:1"));
  }

  /* Phase 2: Add more runtime routes - all peers should receive */
  XLOG(INFO, "Phase 2: Adding more runtime routes");
  injectLocalRoutesAtRuntime({"28.0.0.0/8"}, {"308:1"}, 150);
  injectLocalRoutesAtRuntime({"29.0.0.0/8"}, {"309:1"}, 150);

  for (const auto& peer : allPeers) {
    std::vector<VerifySpec> expectedRuntimeRoutes = {
        {.prefix = "28.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "308:1"},
        {.prefix = "29.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "309:1"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedRuntimeRoutes));
  }

  /* Phase 3: WITHDRAW routes - all peers should get withdrawals */
  XLOG(INFO, "Phase 3: Withdrawing routes");
  withdrawLocalRoutesAtRuntime({"27.0.0.0/8", "28.0.0.0/8"});

  std::vector<WithdrawSpec> expectedWithdraws = {
      {.prefix = "27.0.0.0", .prefixLen = 8},
      {.prefix = "28.0.0.0", .prefixLen = 8}};

  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteWithdraws("v4", peer, expectedWithdraws));
  }

  /* Phase 4: RE-ADD withdrawn routes */
  XLOG(INFO, "Phase 4: Re-adding withdrawn routes");
  injectLocalRoutesAtRuntime({"27.0.0.0/8"}, {"307:2"}, 150);
  injectLocalRoutesAtRuntime({"28.0.0.0/8"}, {"308:2"}, 150);

  for (const auto& peer : allPeers) {
    std::vector<VerifySpec> expectedReAdded = {
        {.prefix = "27.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "307:2"},
        {.prefix = "28.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "308:2"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedReAdded));
  }

  /* Phase 5: CHANGE attributes - all peers should get updates */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"29.0.0.0/8"}, {"309:99"}, 150);

  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "29.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "309:99"));
  }

  XLOG(INFO, "=== TEST PASSED: New peer JOIN ===");
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
