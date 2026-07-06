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
 * E2E tests for BGP Update Group MVP functionality - Single Peer Initial Dump
 * Tests complete flow: RIB → PeerManagerBase → UpdateGroup → Peers
 * Requires: Change List Tracker + Update Group + Egress Backpressure
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * DEBUG: Minimal single peer initial dump test
 */
TEST_P(UpdateGroupInitialDumpTest, DebugOnePeerInitialDump) {
  XLOG(INFO, "=== TEST START: DebugOnePeerInitialDump ===");

  /* Add single peer */
  XLOG(INFO, "Adding peer 127.3.0.1");
  addPeer(kDefaultPeerSpec3);

  /* Add local route to config (before RIB creation) */
  XLOG(INFO, "Adding local route 10.0.0.0/8 to config");
  addLocalRoute("10.0.0.0/8", {"100:1", "100:2"}, 100);

  /* Setup components - RIB will announce the local route */
  XLOG(INFO, "Creating RIB and PeerManagerBase");
  setupComponents();

  /* Bring up peer and send EoR IMMEDIATELY to avoid 45s EoR timer wait */
  XLOG(INFO, "Bringing up peer 127.3.0.1");
  bringUpPeer(kPeerAddr3);

  XLOG(INFO, "Sending ingress EoR to peer 127.3.0.1");
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  /* Wait for route to reach shadowRIB (async flow completes) */
  XLOG(INFO, "Waiting for route to reach shadowRIB");
  auto routePrefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route did not reach shadowRIB in time";

  /* Verify route */
  XLOG(INFO, "Verifying route 10.0.0.0/8 at peer 127.3.0.1");
  bool verified = verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "100:1 100:2");
  XLOGF(INFO, "Route verification: {}", verified ? "PASSED" : "FAILED");
  EXPECT_TRUE(verified);

  /* Wait for egress EoR */
  XLOG(INFO, "Waiting for egress EoR from peer 127.3.0.1");
  bool gotEoR = waitForEoR(peerId3);
  XLOGF(INFO, "Egress EoR: {}", gotEoR ? "RECEIVED" : "TIMEOUT");
  EXPECT_TRUE(gotEoR);

  XLOG(INFO, "=== TEST END: DebugOnePeerInitialDump ===");
}

/*
 * DEBUG: Single iBGP peer initial dump test
 * Test local route announcement to iBGP peer (same ASN as local)
 */
TEST_P(UpdateGroupInitialDumpTest, DebugIbgpSinglePeer) {
  XLOG(INFO, "=== TEST START: DebugIbgpSinglePeer ===");

  /* Create iBGP peer spec (same ASN as local) */
  BgpPeerSpec ibgpSpec = kDefaultPeerSpec3;
  ibgpSpec.asn = kAsn1; /* Make it iBGP - same as local ASN */

  XLOGF(INFO, "Adding iBGP peer 127.3.0.1 with ASN={}", ibgpSpec.asn);
  addPeer(ibgpSpec);

  /* Add local route to config */
  XLOG(INFO, "Adding local route 20.0.0.0/8 to config");
  addLocalRoute("20.0.0.0/8", {"200:1"}, 100);

  /* Setup components */
  XLOG(INFO, "Creating RIB and PeerManagerBase");
  setupComponents();

  /* Bring up peer and send EoR IMMEDIATELY to avoid 45s EoR timer wait */
  XLOG(INFO, "Bringing up iBGP peer 127.3.0.1");
  bringUpPeer(kPeerAddr3);

  XLOG(INFO, "Sending ingress EoR to iBGP peer");
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  /* Wait for route to reach shadowRIB */
  XLOG(INFO, "Waiting for route to reach shadowRIB");
  auto routePrefix = folly::IPAddress::createNetwork("20.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 20.0.0.0/8 did not reach shadowRIB in time";

  /* Verify route - iBGP should receive local routes without AS path */
  XLOG(INFO, "Verifying route 20.0.0.0/8 at iBGP peer 127.3.0.1");
  bool verified = verifyRouteAdd(
      "v4",
      "20.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "", /* iBGP local routes have no AS path */
      "200:1");
  XLOGF(INFO, "iBGP route verification: {}", verified ? "PASSED" : "FAILED");
  EXPECT_TRUE(verified);

  /* Wait for egress EoR */
  XLOG(INFO, "Waiting for egress EoR from iBGP peer");
  bool gotEoR = waitForEoR(peerId3);
  XLOGF(INFO, "Egress EoR: {}", gotEoR ? "RECEIVED" : "TIMEOUT");
  EXPECT_TRUE(gotEoR);

  XLOG(INFO, "=== TEST END: DebugIbgpSinglePeer ===");
}

/*
 * Simplest possible test: 1 peer, update group enabled, no routes
 * From DebugEoRTest.cpp - basic EoR sanity test
 */
TEST_P(UpdateGroupInitialDumpTest, OnePeerNoRoutes) {
  XLOG(INFO, "=== TEST START: OnePeerNoRoutes ===");

  /* Add peer configuration */
  XLOG(INFO, "Adding peer config for 127.3.0.1");
  addPeer(kDefaultPeerSpec3);

  /* Create RIB and PeerManagerBase with update groups */
  XLOG(INFO, "Creating RIB and PeerManagerBase");
  setupComponents();

  /* Bring up peer */
  XLOG(INFO, "Bringing up peer 127.3.0.1");
  bringUpPeer(kPeerAddr3);

  /* Send ingress EoR to peer */
  XLOG(INFO, "Sending ingress EoR to peer 127.3.0.1");
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  /* Wait for egress EoR from peer (0 routes case) */
  XLOG(INFO, "Waiting for egress EoR (0 routes - should be immediate)");
  bool gotEoR = waitForEoR(peerId3);
  XLOGF(INFO, "Egress EoR: {}", gotEoR ? "RECEIVED" : "TIMEOUT");
  EXPECT_TRUE(gotEoR);

  XLOG(INFO, "=== TEST END: OnePeerNoRoutes ===");
}

TEST_P(UpdateGroupInitialDumpTest, SimpleBgpInitialDumpWithUpdateGroup) {
  /* Step 1: Add peers to configuration explicitly */
  XLOG(INFO, "Step 1: Adding peers to configuration");
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Step 2: Add local route to config (before RIB creation) */
  XLOG(INFO, "Step 2: Adding local route 10.0.0.0/8 to config");
  addLocalRoute("10.0.0.0/8", {"100:1", "100:2"}, 100);

  XLOG(INFO, "=== TEST START: SimpleBgpInitialDumpWithUpdateGroup ===");

  /* Step 3: Create RIB and PeerManagerBase infrastructure */
  XLOG(INFO, "Step 3: Creating RIB and PeerManagerBase with update groups");
  setupComponents();

  /* Step 4: Bring up peers and send EoR immediately to avoid EoR timer wait */
  XLOG(INFO, "Step 4: Bringing up peer 127.3.0.1");
  bringUpPeer(kPeerAddr3);
  XLOG(INFO, "Step 4: Bringing up peer 127.4.0.1");
  bringUpPeer(kPeerAddr4);

  XLOG(INFO, "Step 4: Sending EoR to peer 127.3.0.1");
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  XLOG(INFO, "Step 4: Sending EoR to peer 127.4.0.1");
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Step 5: Wait for route to reach shadowRIB */
  XLOG(INFO, "Step 5: Waiting for route to reach shadowRIB");
  auto routePrefix = folly::IPAddress::createNetwork("10.0.0.0/8");
  ASSERT_TRUE(waitForRouteInShadowRib(routePrefix))
      << "Route 10.0.0.0/8 did not reach shadowRIB in time";

  /*
   * Step 6: Verify route at BOTH peers (consume route UPDATEs from ALL peers)
   */
  XLOG(INFO, "Step 6: Verifying route 10.0.0.0/8 at peer3");
  bool routeVerifiedPeer3 = verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001",
      "100:1 100:2");
  XLOGF(
      INFO, "Step 6 result: Route verified at peer3 = {}", routeVerifiedPeer3);
  EXPECT_TRUE(routeVerifiedPeer3);

  XLOG(INFO, "Step 6: Verifying route 10.0.0.0/8 at peer4");
  bool routeVerifiedPeer4 = verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "100:1 100:2");
  XLOGF(
      INFO, "Step 6 result: Route verified at peer4 = {}", routeVerifiedPeer4);
  EXPECT_TRUE(routeVerifiedPeer4);

  /* Step 7: Wait for egress EoR from ALL peers (final PDU after routes) */
  XLOG(INFO, "Step 7: Waiting for EoR from peer3 (final PDU)");
  EXPECT_TRUE(waitForEoR(peerId3));

  XLOG(INFO, "Step 7: Waiting for EoR from peer4 (final PDU)");
  EXPECT_TRUE(waitForEoR(peerId4));

  XLOG(INFO, "=== TEST END: SimpleBgpInitialDumpWithUpdateGroup ===");
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
