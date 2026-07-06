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
 * E2E tests for BGP Update Group MVP functionality - Multi-Group Initial Dump
 * Tests complete flow: RIB → PeerManagerBase → UpdateGroup → Peers
 * Requires: Change List Tracker + Update Group + Egress Backpressure
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

TEST_P(UpdateGroupInitialDumpTest, FivePeersTwoGroups) {
  XLOG(INFO, "=== TEST: 5 peers in 2 different groups ===");

  /* Step 1: Setup peers for both groups */
  /* Group 1: iBGP peers (same ASN as local) */
  BgpPeerSpec spec3 = kDefaultPeerSpec3;
  spec3.asn = kAsn1; // iBGP
  BgpPeerSpec spec4 = kDefaultPeerSpec4;
  spec4.asn = kAsn1; // iBGP
  BgpPeerSpec spec5 = {
      kAsn1, kLocalAddr1, kPeerAddr5, kNextHopV4_5, kNextHopV6_5, ""};
  BgpPeerSpec spec6 = {
      kAsn1, kLocalAddr1, kPeerAddr6, kNextHopV4_6, kNextHopV6_6, ""};
  BgpPeerSpec spec7 = {
      kAsn1, kLocalAddr1, kPeerAddr7, kNextHopV4_7, kNextHopV6_7, ""};

  /* Group 2: eBGP peers (different ASN) */
  BgpPeerSpec spec8 = {
      kEbgpAsn1, kLocalAddr1, kPeerAddr8, kNextHopV4_8, kNextHopV6_8, ""};
  BgpPeerSpec spec9 = {
      kEbgpAsn1, kLocalAddr1, kPeerAddr9, kNextHopV4_9, kNextHopV6_9, ""};
  BgpPeerSpec spec10 = {
      kEbgpAsn1, kLocalAddr1, kPeerAddr10, kNextHopV4_10, kNextHopV6_3, ""};
  BgpPeerSpec spec11 = {
      kEbgpAsn1, kLocalAddr1, kPeerAddr11, kNextHopV4_11, kNextHopV6_3, ""};
  BgpPeerSpec spec12 = {
      kEbgpAsn1, kLocalAddr1, kPeerAddr12, kNextHopV4_12, kNextHopV6_3, ""};

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
  addLocalRoute("22.0.0.0/8", {"302:1"}, 100);

  /* Step 4: Create RIB and PeerManagerBase */
  setupComponents();

  /* Step 5: Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("22.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 22.0.0.0/8 did not reach shadowRIB in time";

  std::vector<folly::IPAddress> ibgpPeers = {
      kPeerAddr3, kPeerAddr4, kPeerAddr5, kPeerAddr6, kPeerAddr7};
  std::vector<folly::IPAddress> ebgpPeers = {
      kPeerAddr8, kPeerAddr9, kPeerAddr10, kPeerAddr11, kPeerAddr12};

  /* Step 6: Bring up all peers */
  for (const auto& peer : ibgpPeers) {
    bringUpPeer(peer);
  }
  for (const auto& peer : ebgpPeers) {
    bringUpPeer(peer);
  }

  /* Step 5: Send EoR to all peers (ingress EoR) */
  for (const auto& peer : ibgpPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    sendEoRToPeer(peerId);
  }
  for (const auto& peer : ebgpPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    sendEoRToPeer(peerId);
  }

  /* Step 6: Consume route UPDATEs - both groups should receive the route */
  /* iBGP peers: local routes have no AS path */
  for (const auto& peer : ibgpPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4", "22.0.0.0", 8, peer, getExpectedNexthop(peer), "", "302:1"));
  }
  /* eBGP peers: AS path prepended with local ASN */
  for (const auto& peer : ebgpPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "22.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "302:1"));
  }

  /* Step 7: Wait for egress EoR from ALL peers (final PDU) */
  for (const auto& peer : ibgpPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    EXPECT_TRUE(waitForEoR(peerId));
  }
  for (const auto& peer : ebgpPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Phase 2: Add runtime routes - both groups should receive */
  XLOG(INFO, "Phase 2: Adding runtime routes");
  injectLocalRoutesAtRuntime({"40.0.0.0/8"}, {"320:1"}, 150);
  injectLocalRoutesAtRuntime({"41.0.0.0/8"}, {"321:1"}, 150);

  /* iBGP peers: no AS path */
  for (const auto& peer : ibgpPeers) {
    std::vector<VerifySpec> ibgpRoutes = {
        {.prefix = "40.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "",
         .expectedCommunity = "320:1"},
        {.prefix = "41.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "",
         .expectedCommunity = "321:1"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, ibgpRoutes));
  }

  /* eBGP peers: AS path prepended */
  for (const auto& peer : ebgpPeers) {
    std::vector<VerifySpec> ebgpRoutes = {
        {.prefix = "40.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "320:1"},
        {.prefix = "41.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "321:1"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, ebgpRoutes));
  }

  /* Phase 3: WITHDRAW routes - both groups should get withdrawals */
  XLOG(INFO, "Phase 3: Withdrawing routes");
  withdrawLocalRoutesAtRuntime({"40.0.0.0/8"});

  for (const auto& peer : ibgpPeers) {
    EXPECT_TRUE(verifyRouteWithdraw("v4", "40.0.0.0", 8, peer));
  }
  for (const auto& peer : ebgpPeers) {
    EXPECT_TRUE(verifyRouteWithdraw("v4", "40.0.0.0", 8, peer));
  }

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding withdrawn route");
  injectLocalRoutesAtRuntime({"40.0.0.0/8"}, {"320:2"}, 150);

  for (const auto& peer : ibgpPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4", "40.0.0.0", 8, peer, getExpectedNexthop(peer), "", "320:2"));
  }
  for (const auto& peer : ebgpPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "40.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "320:2"));
  }

  /* Phase 5: CHANGE attributes */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"41.0.0.0/8"}, {"321:99"}, 150);

  for (const auto& peer : ibgpPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4", "41.0.0.0", 8, peer, getExpectedNexthop(peer), "", "321:99"));
  }
  for (const auto& peer : ebgpPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "41.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "321:99"));
  }

  XLOG(INFO, "=== TEST PASSED: 5+5 peers in 2 groups ===");
}

/*
 * Test 4: 10 peers each in their own update group
 * Use different peer types to create separate groups
 */
TEST_P(UpdateGroupInitialDumpTest, TenPeersEachOwnGroup) {
  XLOG(INFO, "=== TEST: 10 peers each in own group ===");

  /*
   * Step 1: Setup peers - each with different parameters to force separate
   * groups
   */
  /* Using different peerType for each */
  BgpPeerSpec spec3 = kDefaultPeerSpec3;
  spec3.peerType = kPeerTypeCsw;
  BgpPeerSpec spec4 = kDefaultPeerSpec4;
  spec4.peerType = kPeerTypeFa;
  BgpPeerSpec spec5 = {
      kPeerAsn5,
      kLocalAddr1,
      kPeerAddr5,
      kNextHopV4_5,
      kNextHopV6_5,
      "",
      true,
      kPeerTypeShiv};
  BgpPeerSpec spec6 = {
      kPeerAsn6,
      kLocalAddr1,
      kPeerAddr6,
      kNextHopV4_6,
      kNextHopV6_6,
      "",
      true,
      kPeerTypeBgpMonitor};
  BgpPeerSpec spec7 = {
      kPeerAsn7,
      kLocalAddr1,
      kPeerAddr7,
      kNextHopV4_7,
      kNextHopV6_7,
      "",
      true,
      kPeerTypeEdsw};
  BgpPeerSpec spec8 = {
      kPeerAsn8,
      kLocalAddr1,
      kPeerAddr8,
      kNextHopV4_8,
      kNextHopV6_8,
      "",
      true,
      kPeerTypeRdsw};
  /* For remaining peers, use different ASNs to separate them */
  BgpPeerSpec spec9 = {
      65100,
      kLocalAddr1,
      kPeerAddr9,
      kNextHopV4_9,
      kNextHopV6_9,
      "",
      true,
      kPeerTypeCsw};
  BgpPeerSpec spec10 = {
      65101,
      kLocalAddr1,
      kPeerAddr10,
      kNextHopV4_10,
      kNextHopV6_3,
      "",
      true,
      kPeerTypeCsw};
  BgpPeerSpec spec11 = {
      65102,
      kLocalAddr1,
      kPeerAddr11,
      kNextHopV4_11,
      kNextHopV6_3,
      "",
      true,
      kPeerTypeCsw};
  BgpPeerSpec spec12 = {
      65103,
      kLocalAddr1,
      kPeerAddr12,
      kNextHopV4_12,
      kNextHopV6_3,
      "",
      true,
      kPeerTypeCsw};

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
  addLocalRoute("23.0.0.0/8", {"303:1"}, 100);

  /* Step 4: Create RIB and PeerManagerBase */
  setupComponents();

  /* Step 5: Wait for route to reach shadowRIB */
  auto routePrefix = folly::IPAddress::createNetwork("23.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 23.0.0.0/8 did not reach shadowRIB in time";

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

  /* Step 6: Consume route UPDATEs for all peers */
  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "23.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "303:1"));
  }

  /* Step 7: Wait for egress EoR from ALL peers (final PDU) */
  for (const auto& peer : allPeers) {
    BgpPeerId peerId{peer, peer.asV4().toLongHBO()};
    EXPECT_TRUE(waitForEoR(peerId));
  }

  /* Phase 2: Add runtime routes - all 10 groups should receive */
  XLOG(INFO, "Phase 2: Adding runtime routes");
  injectLocalRoutesAtRuntime({"42.0.0.0/8"}, {"322:1"}, 150);
  injectLocalRoutesAtRuntime({"43.0.0.0/8"}, {"323:1"}, 150);

  for (const auto& peer : allPeers) {
    std::vector<VerifySpec> expectedRuntimeRoutes = {
        {.prefix = "42.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "322:1"},
        {.prefix = "43.0.0.0",
         .prefixLen = 8,
         .expectedNexthop = getExpectedNexthop(peer),
         .expectedAsPath = "4200000001",
         .expectedCommunity = "323:1"}};
    EXPECT_TRUE(verifyRoutes("v4", peer, expectedRuntimeRoutes));
  }

  /* Phase 3: WITHDRAW routes - all 10 groups should get withdrawals */
  XLOG(INFO, "Phase 3: Withdrawing routes");
  withdrawLocalRoutesAtRuntime({"42.0.0.0/8"});

  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteWithdraw("v4", "42.0.0.0", 8, peer));
  }

  /* Phase 4: RE-ADD withdrawn route */
  XLOG(INFO, "Phase 4: Re-adding withdrawn route");
  injectLocalRoutesAtRuntime({"42.0.0.0/8"}, {"322:2"}, 150);

  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "42.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "322:2"));
  }

  /* Phase 5: CHANGE attributes */
  XLOG(INFO, "Phase 5: Changing route attributes");
  injectLocalRoutesAtRuntime({"43.0.0.0/8"}, {"323:99"}, 150);

  for (const auto& peer : allPeers) {
    EXPECT_TRUE(verifyRouteAdd(
        "v4",
        "43.0.0.0",
        8,
        peer,
        getExpectedNexthop(peer),
        "4200000001",
        "323:99"));
  }

  XLOG(INFO, "=== TEST PASSED: 10 peers in separate groups ===");
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
