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

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "neteng/fboss/bgp/cpp/common/FeatureFlags.h"

namespace facebook::bgp {
namespace {
using ::testing::Address;
using ::testing::Eq;
using ::testing::IsFalse;
using ::testing::IsTrue;

TEST(FeatureFlagsTest, LoadFeaturesAndVerifyTest) {
  const auto kEnableMyFeature = "enable_my_feature";
  const auto kEnableMyFeature2 = "enable_my_feature2";
  const auto kEnableMyFeature3 = "enable_my_feature3";
  const auto kEnableMyFeature4 = "enable_my_feature4";
  {
    thrift::BgpConfig thriftConfig;
    FeatureFlags::LoadFromThriftConfig(thriftConfig);

    EXPECT_FALSE(FeatureFlags::IsFeatureEnabled(kEnableMyFeature));
  }
  {
    thrift::BgpConfig thriftConfig;
    thriftConfig.bgp_setting_config() = thrift::BgpSettingConfig();
    FeatureFlags::LoadFromThriftConfig(thriftConfig);

    EXPECT_FALSE(FeatureFlags::IsFeatureEnabled(kEnableMyFeature));
  }
  {
    thrift::BgpConfig thriftConfig;
    thriftConfig.bgp_setting_config() = thrift::BgpSettingConfig();
    std::set<std::string> features = {kEnableMyFeature};
    thriftConfig.bgp_setting_config()->features() = std::move(features);
    FeatureFlags::LoadFromThriftConfig(thriftConfig);

    EXPECT_TRUE(FeatureFlags::IsFeatureEnabled(kEnableMyFeature));
  }
  {
    thrift::BgpConfig thriftConfig;
    thriftConfig.bgp_setting_config() = thrift::BgpSettingConfig();
    std::set<std::string> features = {
        kEnableMyFeature, kEnableMyFeature2, kEnableMyFeature3};
    thriftConfig.bgp_setting_config()->features() = std::move(features);
    FeatureFlags::LoadFromThriftConfig(thriftConfig);

    EXPECT_TRUE(FeatureFlags::IsFeatureEnabled(kEnableMyFeature));
    EXPECT_TRUE(FeatureFlags::IsFeatureEnabled(kEnableMyFeature2));
    EXPECT_TRUE(FeatureFlags::IsFeatureEnabled(kEnableMyFeature3));
    EXPECT_FALSE(FeatureFlags::IsFeatureEnabled(kEnableMyFeature4));
  }
}

TEST(FeatureFlagsTest, DefaultBgpBestpathFeaturesTest) {
  // Test default values when no configuration is loaded
  const FeatureFlags::BgpBestpathFeatures& config =
      FeatureFlags::getBgpBestpathFeatures();

  // Default values should be false
  EXPECT_THAT(config.enableMedComparison, IsFalse());
  EXPECT_THAT(config.enableMedMissingAsWorst, IsFalse());
  EXPECT_THAT(config.enableWeightComparison, IsFalse());
  EXPECT_THAT(config.enableNextHopTracking, IsFalse());
  EXPECT_THAT(config.nextHopTrackingUseOpenrIgpCost, IsFalse());
  EXPECT_THAT(config.enableEiBgpMultipath, IsFalse());
}

TEST(FeatureFlagsTest, LoadBgpBestpathFeaturesTest) {
  // Test values after loading a configuration
  thrift::BgpConfig thriftConfig;
  thriftConfig.bgp_setting_config() = thrift::BgpSettingConfig();
  thriftConfig.bgp_setting_config()->enable_med_comparison() = true;
  thriftConfig.bgp_setting_config()->enable_med_missing_as_worst() = true;
  thriftConfig.bgp_setting_config()->enable_weight_comparison() = true;
  thriftConfig.bgp_setting_config()->enable_next_hop_tracking() = true;
  thriftConfig.bgp_setting_config()->next_hop_tracking_use_openr_igp_cost() =
      true;
  thriftConfig.bgp_setting_config()->enable_eibgp_multipath() = true;

  FeatureFlags::LoadFromThriftConfig(thriftConfig);

  const FeatureFlags::BgpBestpathFeatures& config =
      FeatureFlags::getBgpBestpathFeatures();

  // Values should match what we set in the config
  EXPECT_THAT(config.enableMedComparison, IsTrue());
  EXPECT_THAT(config.enableMedMissingAsWorst, IsTrue());
  EXPECT_THAT(config.enableWeightComparison, IsTrue());
  EXPECT_THAT(config.enableNextHopTracking, IsTrue());
  EXPECT_THAT(config.nextHopTrackingUseOpenrIgpCost, IsTrue());
  EXPECT_THAT(config.enableEiBgpMultipath, IsTrue());
}

TEST(FeatureFlagsTest, BgpBestpathFeaturesObjectTest) {
  // Test that the function always returns the same object
  const FeatureFlags::BgpBestpathFeatures& config1 =
      FeatureFlags::getBgpBestpathFeatures();
  const FeatureFlags::BgpBestpathFeatures& config2 =
      FeatureFlags::getBgpBestpathFeatures();

  // Both references should point to the same object
  EXPECT_THAT(std::addressof(config1), Eq(std::addressof(config2)));
}
} // anonymous namespace

} // namespace facebook::bgp
