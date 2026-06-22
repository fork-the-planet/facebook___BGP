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

#include <folly/coro/SharedMutex.h>

#include "configerator/structs/neteng/bgp_policy/thrift/gen-cpp2/rib_policy_types.h"
#include "neteng/fboss/bgp/cpp/BgpServiceBase.h"
#include "neteng/fboss/bgp/cpp/peer/NeighborWatcher.h"
#include "neteng/fboss/bgp/cpp/rib/RibDC.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

namespace facebook::bgp {

class BgpServiceDC : public BgpServiceBase {
 public:
  BgpServiceDC(
      PeerManager& peerMgr,
      std::shared_ptr<ConfigManager> configManager,
      RibDC& rib,
      std::shared_ptr<NeighborWatcher> neighborWatcher,
      Watchdog& watchdog,
      bool enable_thrift_protection);
  ~BgpServiceDC() override = default;

  /**
   * [RibPolicy] aggregate getter/clear
   */
  folly::coro::Task<std::unique_ptr<rib_policy::TRibPolicy>> co_getRibPolicy()
      override;

  folly::coro::Task<void> co_clearRibPolicy() override;

  /**
   * [Route Attribute Policy]
   */
  folly::coro::Task<std::unique_ptr<neteng::fboss::bgp::thrift::TResult>>
  co_setRouteAttributePolicy(
      std::unique_ptr<rib_policy::TRouteAttributePolicy> policy) override;

  folly::coro::Task<std::unique_ptr<rib_policy::TRouteAttributePolicy>>
  co_getRouteAttributePolicy() override;

  folly::coro::Task<void> co_clearRouteAttributePolicy() override;

  /**
   * [Golden Prefixes Policy]
   */
  folly::coro::Task<bool> co_getIsSafeModeOn() override;
  folly::coro::Task<void> co_removeSafeModeFile() override;
  folly::coro::Task<
      std::unique_ptr<neteng::fboss::bgp::thrift::TGoldenPrefixesPolicyStatus>>
  co_getGoldenPrefixesPolicyStatus() override;
  int64_t getGoldenVipsCount() override;

  /**
   * [Path Selection Policy]
   */
  folly::coro::Task<std::unique_ptr<neteng::fboss::bgp::thrift::TResult>>
  co_setPathSelectionPolicy(
      std::unique_ptr<rib_policy::TPathSelectionPolicy> policy) override;

  folly::coro::Task<std::unique_ptr<rib_policy::TPathSelectionPolicy>>
  co_getPathSelectionPolicy() override;

  folly::coro::Task<void> co_clearPathSelectionPolicy() override;

  folly::coro::Task<std::unique_ptr<std::vector<rib_policy::TPathSelector>>>
  co_getActivePathSelectionCriteria(
      std::unique_ptr<std::vector<std::string>> prefixes) override;

  /**
   * [Route Filter Policy — FILE_MODE gating]
   * Override base to reject Thrift-based CRF updates when FILE_MODE is active.
   */
  folly::coro::Task<std::unique_ptr<neteng::fboss::bgp::thrift::TResult>>
  co_setRouteFilterPolicy(
      std::unique_ptr<rib_policy::TRouteFilterPolicy> policy) override;

  folly::coro::Task<void> co_clearRouteFilterPolicy() override;

  /**
   * [Route Filter Policy — File Mode]
   * Refresh CRF policy from the local artifact file.
   * Reads CrfPolicyArtifact, syncs dryrun mode, and applies policy if
   * dryrun=false (FILE_MODE).
   */
  folly::coro::Task<std::unique_ptr<neteng::fboss::bgp::thrift::TResult>>
  co_setCrfPolicyFromFile() override;

  folly::coro::Task<void> co_clearIngressEgressRouteFiltersPolicy() override;
  folly::coro::Task<void> co_clearGoldenPrefixesPolicy() override;
  folly::coro::Task<std::unique_ptr<std::map<std::string, int>>>
  co_getGoldenPrefixSubnetCounts() override;

  /**
   * [Partial Drain]
   */
  folly::coro::Task<
      std::unique_ptr<neteng::fboss::bgp::thrift::TPartialDrainStatus>>
  co_getPartialDrainStatus() override;
  folly::coro::Task<
      std::unique_ptr<neteng::fboss::bgp::thrift::TPartialDrainState>>
  co_getPartialDrainState() override;
  folly::coro::Task<std::unique_ptr<
      std::vector<neteng::fboss::bgp::thrift::TPartiallyDrainedPrefix>>>
  co_getPartiallyDrainedPrefixes() override;

  /**
   * [Local Route Injection] DC-only RPCs. Not declared in BgpServiceBase —
   * Thrift's default behavior applies if a client calls these on a non-DC
   * binary (e.g. BB).
   */
  void addNetwork(
      std::unique_ptr<facebook::neteng::fboss::bgp_attr::TIpPrefix> prefix,
      std::unique_ptr<
          std::vector<facebook::neteng::fboss::bgp_attr::TBgpCommunity>>
          communities) override;

  void delNetwork(
      std::unique_ptr<facebook::neteng::fboss::bgp_attr::TIpPrefix> prefix)
      override;

  void addNetworks(
      std::unique_ptr<std::map<
          facebook::neteng::fboss::bgp_attr::TIpPrefix,
          facebook::neteng::fboss::bgp::thrift::TBgpAttributes>> networks)
      override;

  void delNetworks(
      std::unique_ptr<std::set<facebook::neteng::fboss::bgp_attr::TIpPrefix>>
          prefixes) override;

 private:
  RibDC& ribDC_;
  std::shared_ptr<NeighborWatcher> neighborWatcher_;

  /*
   * DC-typed reference to the same Rib instance that the base class
   * stores via RibBase&. Lets DC-only handlers reach RibDC's CPS
   * surfaces (setPathSelectionPolicy / getPathSelectionPolicy /
   * clearPathSelectionPolicy / getPathSelectionPolicyVersion /
   * getActivePathSelectionCriteria) without a downcast at every call
   * site. Initialized from the ctor parameter; lifetime mirrors rib_.
   */
  RibDC& dcRib_;
  folly::coro::SharedMutex crfPolicyMutex_;
};

} // namespace facebook::bgp
