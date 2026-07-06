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
 * E2E tests for BGP Update Group MVP functionality - Multi-Peer Initial Dump
 * Tests complete flow: RIB → PeerManagerBase → UpdateGroup → Peers
 * Requires: Change List Tracker + Update Group + Egress Backpressure
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Initial dump with peer routes (variant of
 * SimpleBgpInitialDumpWithUpdateGroup) Tests route from peer propagation via
 * update groups
 */
TEST_P(
    UpdateGroupInitialDumpTest,
    SimpleBgpInitialDumpWithUpdateGroupPeerRoutes) {
  /* Add peers to configuration */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  setupComponents();

  /* Bring up peers */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to each peer (ingress EoR) */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Wait for egress EoR from all peers (0 routes case) */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add route from peer3 */
  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "10.1.1.1",
      "65100 65200",
      "100:1 100:2",
      0,
      100,
      50);

  /* Peer4 should receive the route from peer3 via update group */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65100 65200",
      "100:1 100:2"));

  /* Phase 2: Withdraw the route */
  XLOG(INFO, "Phase 2: Withdrawing route from peer3");
  deleteRoute("v4", "10.0.0.0", 8, kPeerAddr3);

  EXPECT_TRUE(verifyRouteWithdraw("v4", "10.0.0.0", 8, kPeerAddr4));

  /* Phase 3: Re-add the route */
  XLOG(INFO, "Phase 3: Re-adding route from peer3");
  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "10.1.1.1",
      "65100 65200",
      "100:1 100:2",
      0,
      100,
      50);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65100 65200",
      "100:1 100:2"));

  /* Phase 4: Change attributes */
  XLOG(INFO, "Phase 4: Changing route attributes (community change)");
  addRoute(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      "10.1.1.1",
      "65100 65200",
      "100:99",
      0,
      100,
      50);

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65100 65200",
      "100:99"));
}

TEST_P(UpdateGroupInitialDumpTest, TenPeersSameGroupInitialDump) {
  XLOG(INFO, "=== TEST: 10 peers same group initial dump ===");

  /* Step 1: Add 10 peers with identical parameters (same update group) */
  BgpPeerSpec spec3 = kDefaultPeerSpec3;
  BgpPeerSpec spec4 = kDefaultPeerSpec4;
  BgpPeerSpec spec5 = {
      kPeerAsn5, kLocalAddr5, kPeerAddr5, kNextHopV4_5, kNextHopV6_5};
  BgpPeerSpec spec6 = {
      kPeerAsn6, kLocalAddr6, kPeerAddr6, kNextHopV4_6, kNextHopV6_6};
  BgpPeerSpec spec7 = {
      kPeerAsn7, kLocalAddr7, kPeerAddr7, kNextHopV4_7, kNextHopV6_7};
  BgpPeerSpec spec8 = {
      kPeerAsn8, kLocalAddr8, kPeerAddr8, kNextHopV4_8, kNextHopV6_8};
  BgpPeerSpec spec9 = {
      kPeerAsn9, kLocalAddr9, kPeerAddr9, kNextHopV4_9, kNextHopV6_9};
  BgpPeerSpec spec10 = {
      kPeerAsn10, kLocalAddr10, kPeerAddr10, kNextHopV4_10, kNextHopV6_3};
  BgpPeerSpec spec11 = {
      kPeerAsn11, kLocalAddr11, kPeerAddr11, kNextHopV4_11, kNextHopV6_3};
  BgpPeerSpec spec12 = {
      kPeerAsn12, kLocalAddr12, kPeerAddr12, kNextHopV4_12, kNextHopV6_3};

  /* Step 2: Add peers to configuration */
  addPeer(spec3);
  addPeer(spec4);
  addPeer(spec5);
  addPeer(spec6);
  addPeer(spec7);
  addPeer(spec8);
  addPeer(spec9);
  addPeer(spec10);
  addPeer(spec11);
  addPeer(spec12);

  /* Step 3: Add local route to config */
  addLocalRoute("20.0.0.0/8", {"300:1", "300:2"}, 100);

  /* Step 4: Create RIB and PeerManagerBase */
  setupComponents();

  /*
   * Step 5: Bring up all 10 peers and send EoR immediately to avoid timer wait
   */
  std::vector<folly::IPAddress> allPeers = {
      kPeerAddr3,
      kPeerAddr4,
      kPeerAddr5,
      kPeerAddr6,
      kPeerAddr7,
      kPeerAddr8,
      kPeerAddr9,
      kPeerAddr10,
      kPeerAddr11,
      kPeerAddr12};

  for (const auto& peer : allPeers) {
    bringUpPeer(peer);
  }

  for (const auto& peer : allPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    sendEoRToPeer(peerId);
  }

  /* Step 6: Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 20.0.0.0/8 did not reach shadowRIB in time";

  /* Step 6: Consume route UPDATEs for all peers */
  for (const auto& peer : peers_) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "20.0.0.0",
        8,
        folly::IPAddress(*peer.peer_addr()),
        *peer.next_hop4(),
        "4200000001",
        "300:1 300:2"));
  }

  /* Step 7: Wait for egress EoR from ALL peers (final PDU) */
  for (const auto& peer : allPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Phase 2: Add runtime routes */
  XLOG(INFO, "Phase 2: Adding runtime routes");
  injectLocalRoutesAtRuntime({"21.0.0.0/8"}, {"800:1"}, 100);
  injectLocalRoutesAtRuntime({"22.0.0.0/8"}, {"800:2"}, 100);

  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("21.0.0.0/8")));
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("22.0.0.0/8")));

  /* Verify all 10 peers received runtime routes (ONLY the 2 NEW routes) */
  for (const auto& peer : peers_) {
    std::vector<VerifySpec> expectedRuntime = {
        {.prefix = "21.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = *peer.next_hop4(),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "800:1"},
        {.prefix = "22.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = *peer.next_hop4(),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "800:2"}};
    EXPECT_TRUE(verifyRoutes(
        "v4", folly::IPAddress(*peer.peer_addr()), expectedRuntime));
  }

  /* Phase 3: WITHDRAW one route */
  XLOG(INFO, "Phase 3: Withdrawing route 21");
  withdrawLocalRoutesAtRuntime({"21.0.0.0/8"});

  std::vector<WithdrawSpec> expectedWithdraws = {
      {.prefix = "21.0.0.0", .prefixLen = 8}};

  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteWithdraws("v4", peer, expectedWithdraws));
  }

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding route 21");
  injectLocalRoutesAtRuntime({"21.0.0.0/8"}, {"800:1"}, 100);

  /* Verify ONLY the re-added route */
  for (const auto& peer : peers_) {
    std::vector<VerifySpec> expectedReAdded = {
        {.prefix = "21.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = *peer.next_hop4(),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "800:1"}};
    EXPECT_TRUE(verifyRoutes(
        "v4", folly::IPAddress(*peer.peer_addr()), expectedReAdded));
  }

  XLOG(INFO, "=== TEST PASSED: 10 peers got initial dump ===");
}

/*
 * Instantiate tests for both serialization modes.
 */
INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupInitialDumpTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
