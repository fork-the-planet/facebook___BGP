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

#include <folly/container/F14Set.h>
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"

/*
 * This file defines the set of all feature flags on bgp++.
 *
 * These flags are set in matryoshka. They correspond to flags defined in
 * .cconf files in matryoshka/bgp_setting directory. The .cconf files
 * are then used to populate the bgp_setting_config of thrift::BgpConfig.
 *
 * In this file, we parse the thrift::BgpConfig and introduce
 * a global set of feature flags in the FeatureFlags namespace.
 * We also include two additional methods to load feature flags
 * and check if features are enabled.
 *
 * Important to note that FeatureFlags::LoadFromThriftConfig should be
 * called before FeatureFlags::IsFeatureEnabled is.
 *
 */

namespace facebook::bgp {

namespace FeatureFlags {

struct BgpBestpathFeatures {
  /*
   * Enable MED comparison in BGP best path selection
   */
  bool enableMedComparison = false;
  /*
   * Enable MED comparison in BGP best path selection
   */
  bool enableMedMissingAsWorst = false;
  /*
   * Enable Weight comparison in BGP best path selection
   */
  bool enableWeightComparison = false;
  /*
   * Enable Next Hop Tracking and IGP Cost Comparison in BGP best path selection
   */
  bool enableNextHopTracking = false;
  /*
   * When next-hop tracking is enabled, derive the IGP cost from the Open/R
   * client's nexthops (ClientID::OPENR) instead of from the resolved fwd
   * nexthops. The cost is the minimum over that client's nexthops; the
   * resolved fwd nexthops are not consulted (no intersection between the
   * Open/R and fwd nexthops). If unset, the legacy behavior (min cost over
   * the resolved fwd nexthops) is used. In either case, if no relevant
   * nexthop carries a cost, the IGP cost is taken as unset (the nexthop stays
   * reachable/eligible, just without an IGP-cost preference).
   *
   * LIMITED-USE-CASE FLAG -- READ BEFORE EXTENDING.
   * This flag is intentionally narrow. It exists only to satisfy the single,
   * concrete use-case understood today: FBOSS deployments that run exactly
   * one IGP (Open/R) and want BGP++ nexthop-tracking IGP cost to come from
   * that IGP's routes rather than from the resolved fwd nexthops. It is a
   * single hard-coded protocol (Open/R) toggle, NOT a general mechanism for
   * selecting an IGP cost source.
   *
   * It deliberately does NOT model multiple IGPs/protocols, a protocol
   * selection policy, or awareness of which protocol contributed the
   * best-path. If/when there is a real need to scale this properly --
   * multiple protocols, an explicit selection policy, and the best-path
   * selected protocol in context -- a proper, well-specified solution should
   * be designed at that point with clear use-cases, and THIS limited flag
   * should be deprecated and removed in favor of it. Do not grow this boolean
   * into that general mechanism.
   */
  bool nextHopTrackingUseOpenrIgpCost = false;
  /*
   * Enable eiBGP feature in best path selection -- skip EXTERNAL_ROUTE
   * preference filter
   */
  bool enableEiBgpMultipath = false;
};

extern BgpBestpathFeatures bgpBestpathFeatures;
extern folly::F14NodeSet<std::string> features;

void LoadFromThriftConfig(const thrift::BgpConfig& bgpThriftConfig);
bool IsFeatureEnabled(const std::string& feature);
const BgpBestpathFeatures& getBgpBestpathFeatures();

} // namespace FeatureFlags

} // namespace facebook::bgp
