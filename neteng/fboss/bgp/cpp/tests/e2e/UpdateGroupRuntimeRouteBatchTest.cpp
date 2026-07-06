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
 * This file contains batch route operation tests.
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupDistributionTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test batch route operations with addRoutes() and verifyRoutes()
 * Demonstrates optional parameter handling
 */
TEST_P(UpdateGroupRuntimeRouteTest, BatchRouteOperations) {
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

  /* Add multiple routes at once using addRoutes() */
  std::vector<E2ETestFixture::RouteSpec> routes = {
      /* Route with full attributes */
      {.prefix = "5.0.0.0",
       .prefixLen = 8,
       .nexthop = "10.2.2.2",
       .asPath = "500 600",
       .community = "700:1 700:2",
       .localPref = 120},
      /* Route with minimal attributes (only nexthop required) */
      {.prefix = "6.0.0.0", .prefixLen = 8, .nexthop = "10.3.3.3"},
      /* Route with AS path but no community */
      {.prefix = "7.0.0.0",
       .prefixLen = 8,
       .nexthop = "10.4.4.4",
       .asPath = "800 900"}};

  addRoutes("v4", kPeerAddr3, routes);

  /* Verify all routes using verifyRoutes() (automatically skips 2 EoRs) */
  std::vector<E2ETestFixture::VerifySpec> verifySpecs = {
      /* Verify route with all attributes */
      {.prefix = "5.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001 500 600",
       .expectedCommunity = "700:1 700:2"},
      /* Verify route with only nexthop (optional fields not matched) */
      {.prefix = "6.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4)},
      /* Verify route with AS path but no community check */
      {.prefix = "7.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001 800 900"}};

  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr4, verifySpecs));
}

/*
 * Test optional parameter behavior
 * - Empty asPath means no AS path attribute matching
 * - Empty community means no community attribute matching
 */
TEST_P(UpdateGroupRuntimeRouteTest, OptionalParameterBehavior) {
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

  /* Add route with community but no AS path */
  addRoute(
      "v4",
      "8.0.0.0",
      8,
      kPeerAddr3,
      "10.5.5.5",
      "", /* empty asPath */
      "900:1 900:2");

  /* Verify with only nexthop and community (AS path not checked) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "8.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "", /* empty expectedAsPath means don't match AS path */
      "900:1 900:2"));

  /* Add route with AS path but no community */
  addRoute(
      "v4",
      "9.0.0.0",
      8,
      kPeerAddr3,
      "10.6.6.6",
      "1000 1100",
      ""); /* empty community */

  /* Verify with only nexthop and AS path (community not checked) */
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "9.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 1000 1100",
      "")); /* empty expectedCommunity means don't match community */

  /* Phase 2: Add more peer routes with different optional parameter combos */
  XLOG(INFO, "Phase 2: Adding more peer routes with optional params");

  /* Route with both AS path and community */
  addRoute(
      "v4", "10.0.0.0", 8, kPeerAddr3, "10.7.7.7", "2000 2100", "901:1 901:2");

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "10.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 2000 2100",
      "901:1 901:2"));

  /* Route with empty both (minimal route) */
  addRoute("v4", "11.0.0.0", 8, kPeerAddr3, "10.8.8.8");

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "11.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      ""));

  /* Phase 3: Update existing routes with different attributes */
  XLOG(INFO, "Phase 3: Updating peer routes with different attributes");

  /* Update route 8.0.0.0 - change community */
  addRoute("v4", "8.0.0.0", 8, kPeerAddr3, "10.5.5.5", "", "900:99");

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "8.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "",
      "900:99"));

  /* Update route 9.0.0.0 - change AS path */
  addRoute("v4", "9.0.0.0", 8, kPeerAddr3, "10.6.6.6", "3000 3100", "");

  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "9.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001 3000 3100",
      ""));
}

/*
 * Runtime Local Route Injection with Update Group
 *
 * Tests injecting local routes at runtime after BGP is already initialized.
 * Routes should flow: RIB → PeerManagerBase → ShadowRIB → UpdateGroup → Peers
 */
TEST_P(UpdateGroupRuntimeRouteTest, RuntimeLocalRoutesWithUpdateGroup) {
  /* Add peers to configuration */
  XLOG(INFO, "Adding peers to configuration");
  addPeer(kDefaultPeerSpec3);
  addPeer(kDefaultPeerSpec4);

  XLOG(INFO, "=== TEST START: RuntimeLocalRoutesWithUpdateGroup ===");

  /* Setup WITH all features enabled */
  XLOG(INFO, "Setting up components with update group enabled");
  setupComponents();

  /* Bring up peers */
  XLOG(INFO, "Bringing up peers");
  bringUpPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr4);

  /* Send EoR to each peer (ingress EoR) */
  XLOG(INFO, "Sending ingress EoR to peers");
  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};
  sendEoRToPeer(peerId3);
  BgpPeerId peerId4{kPeerAddr4, kPeerAddr4.asV4().toLongHBO()};
  sendEoRToPeer(peerId4);

  /* Wait for egress EoR from all peers */
  XLOG(INFO, "Waiting for EoR from peer3");
  EXPECT_TRUE(waitForEoR(peerId3));
  XLOG(INFO, "Waiting for EoR from peer4");
  EXPECT_TRUE(waitForEoR(peerId4));
  XLOG(INFO, "Received EoR from all peers, update group is ready");

  /* Inject local routes at runtime */
  XLOG(INFO, "Injecting 3 local routes at runtime");
  injectLocalRoutesAtRuntime(
      {"11.0.0.0/8", "12.0.0.0/8", "13.0.0.0/8"},
      {"200:1", "200:2"},
      150 /* localPref */);
  XLOG(INFO, "Routes injected, waiting for updates...");

  /* Verify all three routes */
  std::vector<E2ETestFixture::VerifySpec> expectedRoutes = {
      {.prefix = "11.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "200:1 200:2"},
      {.prefix = "12.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "200:1 200:2"},
      {.prefix = "13.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "200:1 200:2"}};

  XLOG(INFO, "Starting route verification");
  bool verified = verifyRoutes("v4", kPeerAddr4, expectedRoutes);
  XLOGF(INFO, "Route verification {}", verified ? "PASSED" : "FAILED");
  EXPECT_TRUE(verified);

  /* Phase 2: Withdraw all routes and verify */
  XLOG(INFO, "Phase 2: Withdrawing all local routes");
  withdrawLocalRoutesAtRuntime({"11.0.0.0/8", "12.0.0.0/8", "13.0.0.0/8"});

  std::vector<E2ETestFixture::WithdrawSpec> expectedWithdrawals = {
      {.prefix = "11.0.0.0", .prefixLen = 8},
      {.prefix = "12.0.0.0", .prefixLen = 8},
      {.prefix = "13.0.0.0", .prefixLen = 8}};

  EXPECT_TRUE(verifyRouteWithdraws("v4", kPeerAddr4, expectedWithdrawals));

  /* Phase 3: Re-add 2 routes */
  XLOG(INFO, "Phase 3: Re-adding 2 routes");
  injectLocalRoutesAtRuntime(
      {"11.0.0.0/8", "12.0.0.0/8"}, {"200:1", "200:2"}, 150);

  std::vector<E2ETestFixture::VerifySpec> expectedReAddRoutes = {
      {.prefix = "11.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "200:1 200:2"},
      {.prefix = "12.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "200:1 200:2"}};

  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr4, expectedReAddRoutes));

  /* Phase 4: Change attributes */
  XLOG(INFO, "Phase 4: Changing route attributes (community change)");
  injectLocalRoutesAtRuntime({"11.0.0.0/8"}, {"200:99"}, 200);

  std::vector<E2ETestFixture::VerifySpec> expectedChangedRoute = {
      {.prefix = "11.0.0.0",
       .prefixLen = 8,
       .expectedNexthop = getExpectedNexthop(kPeerAddr4),
       .expectedAsPath = "4200000001",
       .expectedCommunity = "200:99"}};

  EXPECT_TRUE(verifyRoutes("v4", kPeerAddr4, expectedChangedRoute));

  XLOG(INFO, "=== TEST END: RuntimeLocalRoutesWithUpdateGroup ===");
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
