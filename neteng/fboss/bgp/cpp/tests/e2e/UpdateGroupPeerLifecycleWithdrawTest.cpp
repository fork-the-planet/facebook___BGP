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
 * Test 2: Withdraw route from 10 peers in same update group
 */
TEST_P(UpdateGroupPeerLifecycleTest, TenPeersSameGroupWithdraw) {
  XLOG(INFO, "=== TEST: 10 peers same group withdraw ===");

  /* Step 1: Setup 10 peer specs */
  BgpPeerSpec spec3 = kDefaultPeerSpec3;
  BgpPeerSpec spec4 = kDefaultPeerSpec4;
  BgpPeerSpec spec5 = {
      kPeerAsn5, kLocalAddr1, kPeerAddr5, kNextHopV4_5, kNextHopV6_5};
  BgpPeerSpec spec6 = {
      kPeerAsn6, kLocalAddr1, kPeerAddr6, kNextHopV4_6, kNextHopV6_6};
  BgpPeerSpec spec7 = {
      kPeerAsn7, kLocalAddr1, kPeerAddr7, kNextHopV4_7, kNextHopV6_7};
  BgpPeerSpec spec8 = {
      kPeerAsn8, kLocalAddr1, kPeerAddr8, kNextHopV4_8, kNextHopV6_8};
  BgpPeerSpec spec9 = {
      kPeerAsn9, kLocalAddr1, kPeerAddr9, kNextHopV4_9, kNextHopV6_9};
  BgpPeerSpec spec10 = {
      kPeerAsn10, kLocalAddr1, kPeerAddr10, kNextHopV4_10, kNextHopV6_3};
  BgpPeerSpec spec11 = {
      kPeerAsn11, kLocalAddr1, kPeerAddr11, kNextHopV4_11, kNextHopV6_3};
  BgpPeerSpec spec12 = {
      kPeerAsn12, kLocalAddr1, kPeerAddr12, kNextHopV4_12, kNextHopV6_3};

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
  addLocalRoute("21.0.0.0/8", {"301:1"}, 100);

  /* Step 4: Create RIB and PeerManagerBase */
  setupComponents();

  /* Step 5: Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("21.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 21.0.0.0/8 did not reach shadowRIB in time";

  /* Step 6: Setup all peers vector */
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

  /* Step 4: Bring up all peers */
  for (const auto& peer : allPeers) {
    bringUpPeer(peer);
  }

  /* Step 5: Send EoR to each peer (ingress EoR) */
  for (const auto& peer : allPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    sendEoRToPeer(peerId);
  }

  /* Step 6: Verify all peers got the route */
  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "21.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "301:1"));
  }

  /* Step 7: Wait for egress EoR from ALL peers (final PDU) */
  for (const auto& peer : allPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Phase 2: Add runtime routes - all 10 peers should receive */
  XLOG(INFO, "Phase 2: Adding runtime routes");
  injectLocalRoutesAtRuntime({"38.0.0.0/8"}, {"318:1"}, 150);
  injectLocalRoutesAtRuntime({"39.0.0.0/8"}, {"319:1"}, 150);

  for (const auto& peer : allPeers) {
    std::vector<VerifySpec> expectedRuntimeRoutes = {
        {.prefix = "38.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "318:1"},
        {.prefix = "39.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "319:1"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedRuntimeRoutes));
  }

  /* Phase 3: WITHDRAW routes - serialize to avoid race */
  XLOG(INFO, "Phase 3: Withdrawing routes (serialized)");

  withdrawLocalRoutesAtRuntime({"21.0.0.0/8"});
  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteWithdraws(
        "v4", peer, {{.prefix = "21.0.0.0", .prefixLen = 8}}));
  }

  withdrawLocalRoutesAtRuntime({"38.0.0.0/8"});
  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteWithdraws(
        "v4", peer, {{.prefix = "38.0.0.0", .prefixLen = 8}}));
  }

  /* Phase 4: RE-ADD withdrawn routes */
  XLOG(INFO, "Phase 4: Re-adding withdrawn routes");
  injectLocalRoutesAtRuntime({"21.0.0.0/8"}, {"301:2"}, 150);
  injectLocalRoutesAtRuntime({"38.0.0.0/8"}, {"318:2"}, 150);

  for (const auto& peer : allPeers) {
    std::vector<VerifySpec> expectedReAdded = {
        {.prefix = "21.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "301:2"},
        {.prefix = "38.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "318:2"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedReAdded));
  }

  /* Phase 5: CHANGE attributes - all 10 peers should get updates */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"39.0.0.0/8"}, {"319:99"}, 150);

  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "39.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "319:99"));
  }

  XLOG(INFO, "=== TEST PASSED: withdraw to 10 peers ===");
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
