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
 * This file contains runtime route operation tests.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * FINAL TEST: Add route then DELETE - to isolate withdrawal bug
 * - Two peers in same group
 * - Add route from peer3, verify at peer4 (should pass)
 * - DELETE route from peer3, verify withdrawal at peer4 (should HANG if bug is
 * in withdrawals)
 */
TEST_P(UpdateGroupRuntimeRouteTest, MinimalAddThenDelete) {
  XLOG(INFO, "=== FINAL TEST: Add then Delete to isolate withdrawal bug ===");

  /* Setup two peers */
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);
  setupComponents();
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR */
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  sendEoRToPeer(peerId4);
  EXPECT_TRUE(waitForEoR(peerId3));
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Add route (this should work based on previous tests) */
  XLOG(INFO, "Adding route 66.66.66.0/24");
  addRoute(
      "v4",
      "66.66.66.0",
      24,
      kPeerAddr3,
      "10.1.1.1",
      "100 200 300",
      "400:1 400:2 400:3",
      0,
      100,
      50);

  /* Verify add (should pass) */
  XLOG(INFO, "Verifying route addition at peer4");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "66.66.66.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:1 400:2 400:3"));

  /* NOW DELETE the route */
  XLOG(INFO, "*** CRITICAL: Deleting route 66.66.66.0/24 from peer3 ***");
  deleteRoute("v4", "66.66.66.0", 24, kPeerAddr3);

  /* Verify withdrawal at peer4 - THIS IS WHERE IT SHOULD HANG */
  XLOG(INFO, "*** If test hangs here, withdrawal propagation is broken! ***");
  bool withdrawalSeen = verifyRouteWithdraw("v4", "66.66.66.0", 24, kPeerAddr4);

  XLOGF(
      INFO,
      "Withdrawal verification: {}",
      withdrawalSeen ? "PASSED" : "FAILED");
  EXPECT_TRUE(withdrawalSeen);

  /* Phase 3: Re-add the route */
  XLOG(INFO, "Phase 3: Re-adding route 66.66.66.0/24");
  addRoute(
      "v4",
      "66.66.66.0",
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
      "66.66.66.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:1 400:2 400:3"));

  /* Phase 4: Change attributes */
  XLOG(INFO, "Phase 4: Changing route attributes (community change)");
  addRoute(
      "v4",
      "66.66.66.0",
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
      "66.66.66.0",
      24,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:99"));

  XLOG(INFO, "=== FINAL TEST END ===");
}

/*
 * IPv4 Route Add/Delete with Update Group
 *
 * Tests the helper APIs with update groups:
 * - addRoute() with optional AS path, community, nexthop
 * - deleteRoute()
 * - verifyRouteAdd() with optional attribute checking
 * - verifyRouteWithdraw()
 */
TEST_P(
    UpdateGroupRuntimeRouteTest,
    IPv4RouteAddDeleteWithChangeListAndBackpressure) {
  XLOG(
      INFO,
      "=== TEST START: IPv4RouteAddDeleteWithChangeListAndBackpressure ===");

  /* Step 1: Add peers to configuration */
  XLOG(INFO, "Step 1: Adding peers to configuration");
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  /* Step 2: Create RIB and PeerManagerBase */
  XLOG(INFO, "Step 2: Creating RIB and PeerManagerBase");
  setupComponents();

  /* Step 3: Bring up peers */
  XLOG(INFO, "Step 3: Bringing up peer3");
  bringUpPeer(kPeerAddr3);
  XLOG(INFO, "Step 3: Bringing up peer4");
  bringUpPeer(kPeerAddr4);

  /* Step 4: Send EoR to each peer (ingress EoR - unblock BGP initialization) */
  XLOG(INFO, "Step 4: Sending ingress EoR to peer3");
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);

  XLOG(INFO, "Step 4: Sending ingress EoR to peer4");
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Step 5: Wait for egress EoR from both peers (0 routes case) */
  XLOG(INFO, "Step 5: Waiting for egress EoR from peer3");
  EXPECT_TRUE(waitForEoR(peerId3));
  XLOG(INFO, "Step 5: Waiting for egress EoR from peer4");
  EXPECT_TRUE(waitForEoR(peerId4));

  /* Step 6: Add route with full attributes from peer3 at runtime */
  XLOG(INFO, "Step 6: Adding route from peer3");
  addRoute(
      "v4",
      "4.0.0.0",
      8,
      kPeerAddr3,
      "10.1.1.1",
      "100 200 300",
      "400:1 400:2 400:3",
      0,
      100,
      50);

  /* Step 7: Verify route announcement at peer4 with all attributes */
  XLOG(INFO, "Step 7: Verifying route at peer4");
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "4.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:1 400:2 400:3"));

  /* Step 8: Delete route from peer3 */
  XLOG(INFO, "Step 8: Deleting route from peer3");
  deleteRoute("v4", "4.0.0.0", 8, kPeerAddr3);

  /* Step 9: Verify route withdrawal at peer4 */
  XLOG(INFO, "Step 9: Verifying withdrawal at peer4");
  EXPECT_TRUE(verifyRouteWithdraw("v4", "4.0.0.0", 8, kPeerAddr4));

  /* Phase 3: Re-add the route */
  XLOG(INFO, "Phase 3: Re-adding route 4.0.0.0/8");
  addRoute(
      "v4",
      "4.0.0.0",
      8,
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
      "4.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:1 400:2 400:3"));

  /* Phase 4: Change attributes */
  XLOG(INFO, "Phase 4: Changing route attributes (community change)");
  addRoute(
      "v4",
      "4.0.0.0",
      8,
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
      "4.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 100 200 300",
      "400:99"));

  XLOG(
      INFO,
      "=== TEST END: IPv4RouteAddDeleteWithChangeListAndBackpressure ===");
}

/*
 * IPv6 Route Operations with Update Group
 *
 * Tests IPv6 prefix handling with update groups
 */
TEST_P(
    UpdateGroupRuntimeRouteTest,
    IPv6RouteOperationsWithChangeListAndBackpressure) {
  /* Add peers to configuration */
  addPeer(kDefaultPeerSpec3_v6);
  addPeer(kDefaultPeerSpec4_v6);

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

  /* Add IPv6 route */
  addRoute(
      "v6",
      "2001:db8::",
      32,
      kPeerAddr3,
      "2001:db8::1",
      "65000 65001",
      "600:1 600:2",
      0,
      150,
      0);

  /* Verify IPv6 route at peer4 */
  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8::",
      32,
      kPeerAddr4,
      kNextHopV6_4.str(),
      "4200000001 65000 65001",
      "600:1 600:2"));

  /* Delete IPv6 route */
  deleteRoute("v6", "2001:db8::", 32, kPeerAddr3);

  /* Verify IPv6 withdrawal */
  EXPECT_TRUE(verifyRouteWithdraw("v6", "2001:db8::", 32, kPeerAddr4));

  /* Phase 3: Re-add the IPv6 route */
  XLOG(INFO, "Phase 3: Re-adding IPv6 route");
  addRoute(
      "v6",
      "2001:db8::",
      32,
      kPeerAddr3,
      "2001:db8::1",
      "65000 65001",
      "600:1 600:2",
      0,
      150,
      0);

  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8::",
      32,
      kPeerAddr4,
      kNextHopV6_4.str(),
      "4200000001 65000 65001",
      "600:1 600:2"));

  /* Phase 4: Change IPv6 route attributes */
  XLOG(INFO, "Phase 4: Changing IPv6 route attributes");
  addRoute(
      "v6",
      "2001:db8::",
      32,
      kPeerAddr3,
      "2001:db8::1",
      "65000 65001",
      "600:99",
      0,
      150,
      0);

  EXPECT_TRUE(verifyRouteAdd(
      "v6",
      "2001:db8::",
      32,
      kPeerAddr4,
      kNextHopV6_4.str(),
      "4200000001 65000 65001",
      "600:99"));
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
