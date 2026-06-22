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

#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"
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

BgpBestpathFeatures bgpBestpathFeatures;
folly::F14NodeSet<std::string> features;

void LoadFromThriftConfig(const thrift::BgpConfig& bgpThriftConfig) {
  const auto& bgpSettingConfig = bgpThriftConfig.bgp_setting_config();
  if (!bgpSettingConfig) {
    return;
  }
  for (const auto& feature : *bgpSettingConfig->features()) {
    features.insert(feature);
  }
  if (bgpSettingConfig->enable_med_comparison().has_value()) {
    bgpBestpathFeatures.enableMedComparison =
        bgpSettingConfig->enable_med_comparison().value();
  }
  if (bgpSettingConfig->enable_med_missing_as_worst().has_value()) {
    bgpBestpathFeatures.enableMedMissingAsWorst =
        bgpSettingConfig->enable_med_missing_as_worst().value();
  }
  if (bgpSettingConfig->enable_weight_comparison().has_value()) {
    bgpBestpathFeatures.enableWeightComparison =
        bgpSettingConfig->enable_weight_comparison().value();
  }
  if (bgpSettingConfig->enable_next_hop_tracking().has_value()) {
    bgpBestpathFeatures.enableNextHopTracking =
        bgpSettingConfig->enable_next_hop_tracking().value();
  }
  if (bgpSettingConfig->next_hop_tracking_use_openr_igp_cost().has_value()) {
    bgpBestpathFeatures.nextHopTrackingUseOpenrIgpCost =
        bgpSettingConfig->next_hop_tracking_use_openr_igp_cost().value();
  }
  if (bgpSettingConfig->enable_eibgp_multipath().has_value()) {
    bgpBestpathFeatures.enableEiBgpMultipath =
        bgpSettingConfig->enable_eibgp_multipath().value();
  }
}

bool IsFeatureEnabled(const std::string& feature) {
  return features.contains(feature);
}

const BgpBestpathFeatures& getBgpBestpathFeatures() {
  return bgpBestpathFeatures;
}

} // namespace FeatureFlags

} // namespace facebook::bgp
