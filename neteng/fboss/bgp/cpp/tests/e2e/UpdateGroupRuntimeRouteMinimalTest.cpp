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
 * E2E tests for BGP Update Group MVP functionality - Runtime Route scenarios
 * Tests complete flow: RIB → PeerManagerBase → UpdateGroup → Peers
 * Requires: Change List Tracker + Update Group + Egress Backpressure
 *
 * This file contains minimal runtime route tests.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * MINIMAL TEST: Isolate the runtime route distribution hang
 * - One peer, no initial routes
 * - Peer comes up, sends EoR, waits for EoR
 * - Add ONE route at runtime
 * - Verify route appears (THIS IS WHERE IT HANGS)
 */
TEST_P(UpdateGroupRuntimeRouteTest, MinimalRuntimeRoute) {
  XLOG(INFO, "=== MINIMAL TEST START: One peer, one runtime route ===");

  /* Add peer configuration */
  XLOG(INFO, "Adding peer config for 127.3.0.1");
  addPeer(kDefaultPeerSpec3);

  /* Create RIB and PeerManagerBase with update groups - NO INITIAL ROUTES */
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
  XLOG(INFO, "Waiting for egress EoR (should be immediate - no routes)");
  bool gotEoR = waitForEoR(peerId3);
  XLOGF(INFO, "Egress EoR: {}", gotEoR ? "RECEIVED" : "TIMEOUT");
  EXPECT_TRUE(gotEoR);

  /* NOW add ONE route at RUNTIME */
  XLOG(INFO, "Adding ONE route at RUNTIME: 99.99.99.0/24");
  addRoute(
      "v4", "99.99.99.0", 24, kPeerAddr3, "10.1.1.1", "65100 65200", "999:1");

  /* Verify route appears at peer3 - THIS IS THE CRITICAL POINT */
  XLOG(INFO, "Verifying route 99.99.99.0/24 appears at peer 127.3.0.1");
  XLOG(INFO, "*** If test hangs here, runtime route flow is broken ***");
  bool verified = verifyRouteAdd(
      "v4",
      "99.99.99.0",
      24,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001 65100 65200",
      "999:1");

  XLOGF(INFO, "Route verification: {}", verified ? "PASSED" : "FAILED");
  EXPECT_TRUE(verified);

  /* Phase 2: Withdraw the route and verify */
  XLOG(INFO, "Phase 2: Withdrawing route 99.99.99.0/24");
  deleteRoute("v4", "99.99.99.0", 24, kPeerAddr3);

  XLOG(INFO, "Verifying withdrawal received at peer");
  bool withdrawVerified =
      verifyRouteWithdraw("v4", "99.99.99.0", 24, kPeerAddr3);
  XLOGF(
      INFO,
      "Withdraw verification: {}",
      withdrawVerified ? "PASSED" : "FAILED");
  EXPECT_TRUE(withdrawVerified);

  /* Phase 3: Re-add the route with same attributes */
  XLOG(INFO, "Phase 3: Re-adding route 99.99.99.0/24 with same attributes");
  addRoute(
      "v4", "99.99.99.0", 24, kPeerAddr3, "10.1.1.1", "65100 65200", "999:1");

  XLOG(INFO, "Verifying re-added route appears at peer");
  bool readdVerified = verifyRouteAdd(
      "v4",
      "99.99.99.0",
      24,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001 65100 65200",
      "999:1");
  XLOGF(INFO, "Re-add verification: {}", readdVerified ? "PASSED" : "FAILED");
  EXPECT_TRUE(readdVerified);

  /* Phase 4: Change route attributes and verify */
  XLOG(INFO, "Phase 4: Changing route attributes (community 999:1 -> 999:2)");
  addRoute(
      "v4", "99.99.99.0", 24, kPeerAddr3, "10.1.1.1", "65100 65200", "999:2");

  XLOG(INFO, "Verifying attribute change received at peer");
  bool attrChangeVerified = verifyRouteAdd(
      "v4",
      "99.99.99.0",
      24,
      kPeerAddr3,
      getExpectedNexthop(kPeerAddr3),
      "4200000001 65100 65200",
      "999:2");
  XLOGF(
      INFO,
      "Attribute change verification: {}",
      attrChangeVerified ? "PASSED" : "FAILED");
  EXPECT_TRUE(attrChangeVerified);

  XLOG(INFO, "=== MINIMAL TEST END ===");
}

/*
 * CRITICAL TEST: Verify route distribution to OTHER peers in group
 * - Two peers (peer3 and peer4) in same update group
 * - Add route from peer3
 * - Verify route appears at peer4 (NOT originating peer)
 * - This SHOULD hang if bug is "routes don't flow to other peers in group"
 */
TEST_P(UpdateGroupRuntimeRouteTest, MinimalRuntimeRouteTwoPeers) {
  XLOG(INFO, "=== CRITICAL TEST: Runtime route to OTHER peer in group ===");

  /* Add TWO peers */
  XLOG(INFO, "Adding peer3 and peer4 configs");
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Create RIB and PeerManagerBase - NO INITIAL ROUTES */
  XLOG(INFO, "Creating RIB and PeerManagerBase");
  setupComponents();

  /* Bring up BOTH peers */
  XLOG(INFO, "Bringing up peer3 and peer4");
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to both */
  XLOG(INFO, "Sending ingress EoR to both peers");
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Wait for egress EoR from both (0 routes) */
  XLOG(INFO, "Waiting for egress EoR from both peers");
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add ONE route at RUNTIME from peer3 */
  XLOG(INFO, "Adding route FROM peer3");
  addRoute(
      "v4",
      "88.88.88.0",
      24,
      kPeerAddr3, // FROM peer3
      "10.1.1.1",
      "65100 65200",
      "888:1");

  /* CRITICAL: Verify route appears at peer4 (the OTHER peer) */
  XLOG(
      INFO,
      "*** CRITICAL: Verifying route at peer4 (NOT originating peer) ***");
  XLOG(
      INFO, "*** If this hangs, routes don't flow to other peers in group ***");
  bool verified = verifyRouteAdd(
      "v4",
      "88.88.88.0",
      24,
      kPeerAddr4, // AT peer4 (different from peer3!)
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65100 65200",
      "888:1");

  XLOGF(INFO, "Route at peer4: {}", verified ? "FOUND" : "MISSING");
  EXPECT_TRUE(verified);

  /* Phase 2: Withdraw the route and verify both peers receive withdrawal */
  XLOG(INFO, "Phase 2: Withdrawing route 88.88.88.0/24 from peer3");
  deleteRoute("v4", "88.88.88.0", 24, kPeerAddr3);

  XLOG(INFO, "Verifying withdrawal received at peer4");
  bool withdrawVerified =
      verifyRouteWithdraw("v4", "88.88.88.0", 24, kPeerAddr4);
  XLOGF(
      INFO,
      "Withdraw verification at peer4: {}",
      withdrawVerified ? "PASSED" : "FAILED");
  EXPECT_TRUE(withdrawVerified);

  /* Phase 3: Re-add the route and verify */
  XLOG(INFO, "Phase 3: Re-adding route 88.88.88.0/24 from peer3");
  addRoute(
      "v4", "88.88.88.0", 24, kPeerAddr3, "10.1.1.1", "65100 65200", "888:1");

  XLOG(INFO, "Verifying re-added route at peer4");
  bool readdVerified = verifyRouteAdd(
      "v4",
      "88.88.88.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65100 65200",
      "888:1");
  XLOGF(
      INFO,
      "Re-add verification at peer4: {}",
      readdVerified ? "PASSED" : "FAILED");
  EXPECT_TRUE(readdVerified);

  /* Phase 4: Change attributes and verify */
  XLOG(INFO, "Phase 4: Changing attributes (community 888:1 -> 888:2)");
  addRoute(
      "v4", "88.88.88.0", 24, kPeerAddr3, "10.1.1.1", "65100 65200", "888:2");

  XLOG(INFO, "Verifying attribute change at peer4");
  bool attrChangeVerified = verifyRouteAdd(
      "v4",
      "88.88.88.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 65100 65200",
      "888:2");
  XLOGF(
      INFO,
      "Attribute change verification at peer4: {}",
      attrChangeVerified ? "PASSED" : "FAILED");
  EXPECT_TRUE(attrChangeVerified);

  XLOG(INFO, "=== CRITICAL TEST END ===");
}

/*
 * SMOKING GUN TEST: Add route WITH optional params (localPref, med,
 * origOriginatorId)
 * - Two peers in same group
 * - Add route WITH the extra params that hanging test uses
 * - If this hangs, we found the bug!
 */
TEST_P(UpdateGroupRuntimeRouteTest, MinimalWithOptionalParams) {
  XLOG(INFO, "=== SMOKING GUN: Route with localPref/med/origOriginatorId ===");

  /* Add TWO peers */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Setup */
  setupComponents();

  /* Bring up both */
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);

  /* Wait for EoR */
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add route WITH optional params (EXACTLY like hanging test) */
  XLOG(INFO, "Adding route with origOriginatorId=0, localPref=100, med=50");
  addRoute(
      "v4",
      "77.77.77.0",
      24,
      kPeerAddr3,
      "10.1.1.1",
      "100 200 300",
      "400:1 400:2 400:3",
      0, // ← origOriginatorId
      100, // ← localPref
      50); // ← med

  /* Verify at peer4 */
  XLOG(INFO, "*** If this hangs, optional params cause the bug! ***");
  bool verified = verifyRouteAdd(
      "v4",
      "77.77.77.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:1 400:2 400:3");

  XLOGF(
      INFO,
      "Route verification: {}",
      verified ? "PASSED (optional params OK)" : "FAILED");
  EXPECT_TRUE(verified);

  /* Phase 2: Withdraw the route and verify */
  XLOG(INFO, "Phase 2: Withdrawing route 77.77.77.0/24");
  deleteRoute("v4", "77.77.77.0", 24, kPeerAddr3);

  XLOG(INFO, "Verifying withdrawal received at peer4");
  EXPECT_TRUE(verifyRouteWithdraw("v4", "77.77.77.0", 24, kPeerAddr4));

  /* Phase 3: Re-add the route with same attributes */
  XLOG(INFO, "Phase 3: Re-adding route 77.77.77.0/24 with optional params");
  addRoute(
      "v4",
      "77.77.77.0",
      24,
      kPeerAddr3,
      "10.1.1.1",
      "100 200 300",
      "400:1 400:2 400:3",
      0,
      100,
      50);

  XLOG(INFO, "Verifying re-added route at peer4");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.77.77.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:1 400:2 400:3"));

  /* Phase 4: Change route attributes (modify community) */
  XLOG(INFO, "Phase 4: Changing route attributes (community 400:x -> 400:99)");
  addRoute(
      "v4",
      "77.77.77.0",
      24,
      kPeerAddr3,
      "10.1.1.1",
      "100 200 300",
      "400:99",
      0,
      100,
      50);

  XLOG(INFO, "Verifying attribute change at peer4");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "77.77.77.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:99"));

  XLOG(INFO, "=== SMOKING GUN TEST END ===");
}

/*
 * Instantiate tests for both serialization modes.
 */
INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupRuntimeRouteTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
