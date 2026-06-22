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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfo.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopStatus.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace facebook::bgp;
using namespace testing;

class NexthopInfoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create a NexthopStatus with a known IGP cost
    folly::IPAddress nexthopIp("2620:0:1cff:dead:bef1:ffff:ffff:1");
    nexthopStatus_ = std::make_unique<NexthopStatus>(nexthopIp, true, 100);

    // Create a NexthopInfo with the nexthop status
    nexthopInfo_ = std::make_unique<NexthopInfo>(*nexthopStatus_);
  }

  std::unique_ptr<NexthopStatus> nexthopStatus_;
  std::unique_ptr<NexthopInfo> nexthopInfo_;
};

// Test that RouteInfo correctly gets IGP cost from NexthopInfo
TEST_F(NexthopInfoTest, RouteInfoGetsIgpCostFromNexthopInfo) {
  // Create a RouteInfo
  auto routeInfo = createRouteInfo(
      kV4Prefix1,
      kPeerAddr2,
      kPeerAddr1,
      kLocalPref,
      {},
      kPeerAsn2,
      kPeerRouterId2);

  // Initially, the IGP cost should be max value since no NexthopInfo is linked
  EXPECT_EQ(routeInfo->getIgpCostValue(), std::numeric_limits<uint32_t>::max());

  // Link the RouteInfo to the NexthopInfo
  nexthopInfo_->linkRouteInfo(*routeInfo);

  // Now the IGP cost should come from the NexthopInfo
  EXPECT_EQ(routeInfo->getIgpCostValue(), 100);

  // Unlink the RouteInfo
  nexthopInfo_->unlinkRouteInfo(*routeInfo);

  // After unlinking, the IGP cost should be max value again
  EXPECT_EQ(routeInfo->getIgpCostValue(), std::numeric_limits<uint32_t>::max());
}

// Test that NexthopInfo correctly tracks linked RouteInfo objects
TEST_F(NexthopInfoTest, NexthopInfoTracksLinkedRouteInfo) {
  // Create RouteInfo objects
  auto routeInfo1 = createRouteInfo(
      kV4Prefix1,
      kPeerAddr2,
      kPeerAddr1,
      kLocalPref,
      {},
      kPeerAsn2,
      kPeerRouterId2);

  auto routeInfo2 = createRouteInfo(
      kV6Prefix1,
      kPeerAddr2,
      kPeerAddr1,
      kLocalPref,
      {},
      kPeerAsn2,
      kPeerRouterId2);

  // Initially, the association list should be empty
  EXPECT_EQ(nexthopInfo_->getRouteInfoListSize(), 0);

  // Link the first RouteInfo
  nexthopInfo_->linkRouteInfo(*routeInfo1);
  EXPECT_EQ(nexthopInfo_->getRouteInfoListSize(), 1);

  // Link the second RouteInfo
  nexthopInfo_->linkRouteInfo(*routeInfo2);
  EXPECT_EQ(nexthopInfo_->getRouteInfoListSize(), 2);

  // Unlink the first RouteInfo
  nexthopInfo_->unlinkRouteInfo(*routeInfo1);
  EXPECT_EQ(nexthopInfo_->getRouteInfoListSize(), 1);

  // Unlink the second RouteInfo
  nexthopInfo_->unlinkRouteInfo(*routeInfo2);
  EXPECT_EQ(nexthopInfo_->getRouteInfoListSize(), 0);
}

/*
 * isResolvedForSelection() decouples best-path eligibility from the
 * cost-derived isReachable(): a reachable nexthop with no IGP cost is still
 * reported unreachable (so ODS/CLI see the cost-not-set case unchanged), yet it
 * stays eligible for selection only when excludeNexthopWithoutCost is disabled
 * (FBOSS), and is excluded when it is enabled (EBB / default).
 */
TEST_F(NexthopInfoTest, IsResolvedForSelection) {
  folly::IPAddress nh("2620:0:1cff:dead:bef1:ffff:ffff:2");

  // Reachable with an IGP cost: resolved regardless of policy.
  const NexthopStatus reachableWithCost(
      nh, /*isReachable=*/true, /*igpCost=*/10);
  EXPECT_TRUE(reachableWithCost.isReachable());
  EXPECT_TRUE(reachableWithCost.isResolvedForSelection());

  /*
   * Reachable, no cost, EBB policy (default true): reported unreachable AND
   * excluded from selection -- identical to today's cost-not-set behavior.
   */
  const NexthopStatus ebbNoCost(
      nh,
      /*isReachable=*/true,
      /*igpCost=*/std::nullopt,
      /*isConnected=*/std::nullopt,
      /*excludeNexthopWithoutCost=*/true);
  EXPECT_FALSE(ebbNoCost.isReachable());
  EXPECT_FALSE(ebbNoCost.isResolvedForSelection());

  /*
   * Reachable, no cost, FBOSS policy (false): still reported unreachable to
   * ODS/CLI, but resolved for best-path selection.
   */
  const NexthopStatus fbossNoCost(
      nh,
      /*isReachable=*/true,
      /*igpCost=*/std::nullopt,
      /*isConnected=*/std::nullopt,
      /*excludeNexthopWithoutCost=*/false);
  EXPECT_FALSE(fbossNoCost.isReachable());
  EXPECT_TRUE(fbossNoCost.isResolvedForSelection());

  // Unreachable: excluded under either policy.
  const NexthopStatus unreachable(
      nh,
      /*isReachable=*/false,
      /*igpCost=*/std::nullopt,
      /*isConnected=*/std::nullopt,
      /*excludeNexthopWithoutCost=*/false);
  EXPECT_FALSE(unreachable.isReachable());
  EXPECT_FALSE(unreachable.isResolvedForSelection());
}

/*
 * RouteInfo::isResolvedForSelection() reflects the linked nexthop's policy: an
 * unlinked non-local route is not resolved, a route linked to a reachable
 * FBOSS-policy nexthop with no cost is resolved (despite reporting
 * unreachable), and one linked to an EBB-policy no-cost nexthop is excluded.
 */
TEST_F(NexthopInfoTest, RouteInfoResolvedForSelectionFromNexthop) {
  auto routeInfo = createRouteInfo(
      kV4Prefix1,
      kPeerAddr2,
      kPeerAddr1,
      kLocalPref,
      {},
      kPeerAsn2,
      kPeerRouterId2);
  folly::IPAddress nh("2620:0:1cff:dead:bef1:ffff:ffff:3");

  // Non-local route with no linked nexthop: not resolved.
  EXPECT_FALSE(routeInfo->getIsRouteLocal());
  EXPECT_FALSE(routeInfo->isResolvedForSelection());

  /*
   * Reachable-no-cost FBOSS nexthop: resolved for selection even though the
   * nexthop reports unreachable (cost unset).
   */
  const NexthopStatus fbossNoCost(
      nh,
      /*isReachable=*/true,
      /*igpCost=*/std::nullopt,
      /*isConnected=*/std::nullopt,
      /*excludeNexthopWithoutCost=*/false);
  NexthopInfo fbossInfo(fbossNoCost);
  fbossInfo.linkRouteInfo(*routeInfo);
  EXPECT_FALSE(routeInfo->isNextHopReachable());
  EXPECT_TRUE(routeInfo->isResolvedForSelection());
  fbossInfo.unlinkRouteInfo(*routeInfo);

  // Reachable-no-cost EBB nexthop: excluded from selection.
  const NexthopStatus ebbNoCost(
      nh,
      /*isReachable=*/true,
      /*igpCost=*/std::nullopt,
      /*isConnected=*/std::nullopt,
      /*excludeNexthopWithoutCost=*/true);
  NexthopInfo ebbInfo(ebbNoCost);
  ebbInfo.linkRouteInfo(*routeInfo);
  EXPECT_FALSE(routeInfo->isResolvedForSelection());
  ebbInfo.unlinkRouteInfo(*routeInfo);
}
