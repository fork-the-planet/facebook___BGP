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

#pragma once

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/common/RouteInfo.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopAssociationList.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopInfoBase.h"
#include "neteng/fboss/bgp/cpp/nexthopTracker/NexthopStatus.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

namespace facebook::bgp {

class NexthopInfo : public NexthopInfoBase {
 public:
  explicit NexthopInfo(const NexthopStatus& status) : status_(status) {}

  // Delete copy constructor and assignment operator since
  // NexthopAssociationList is not copyable
  NexthopInfo(const NexthopInfo&) = delete;
  NexthopInfo& operator=(const NexthopInfo&) = delete;

  // Allow move operations
  NexthopInfo(NexthopInfo&&) noexcept = default;
  NexthopInfo& operator=(NexthopInfo&&) = delete;

  // Virtual destructor since this class is inherited from NexthopInfoBase
  ~NexthopInfo() override = default;

  /*
   * [Accessor Methods]
   */
  const folly::IPAddress& getNextHop() const {
    return status_.getNexthop();
  }

  bool isReachable() const {
    return status_.isReachable();
  }

  std::optional<uint32_t> getIgpCost() const override {
    return status_.getIgpCost();
  }

  std::optional<bool> isConnected() const override {
    return status_.isConnected();
  }

  bool isResolvedForSelection() const override {
    return status_.isResolvedForSelection();
  }

  /**
   * @brief Update the status of this nexthop
   * @param status The new status for this nexthop
   */
  void updateStatus(const NexthopStatus& status) {
    status_ = status;
  }

  void linkRouteInfo(RouteInfo& routeInfo) {
    routeInfo.setNexthopInfo(this);
    nexthopAssociationList_.link(routeInfo);
    if (!isReachable()) {
      RibStats::incrInactivePathCount(1);
    }
  }

  void unlinkRouteInfo(RouteInfo& routeInfo) {
    routeInfo.setNexthopInfo(nullptr);
    nexthopAssociationList_.unlink(routeInfo);
    if (!isReachable()) {
      RibStats::decrInactivePathCount(1);
    }
  }

  uint32_t getRouteInfoListSize() const {
    return nexthopAssociationList_.size();
  }

  /**
   * @brief Get an iterator to the beginning of the RouteInfo list
   * @return Iterator to the beginning of the RouteInfo list
   */
  auto begin() const {
    return nexthopAssociationList_.begin();
  }

  /**
   * @brief Get an iterator to the end of the RouteInfo list
   * @return Iterator to the end of the RouteInfo list
   */
  auto end() const {
    return nexthopAssociationList_.end();
  }

 private:
  // Nexthop status containing IP address and IGP cost
  NexthopStatus status_;
  // List of routes associated with the nexthop
  NexthopAssociationList nexthopAssociationList_;
};

} // namespace facebook::bgp
