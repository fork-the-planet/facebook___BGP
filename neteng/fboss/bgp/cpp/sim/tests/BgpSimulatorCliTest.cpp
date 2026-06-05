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
 * Smoke tests for the bgp_simulator CLI helpers: config-path expansion and the
 * load -> resolve -> run -> dump pipeline.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "neteng/fboss/bgp/cpp/sim/BgpSimulatorCli.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

class BgpSimulatorCliTest : public ::testing::Test {};

/*
 * Running a single sample config exits 0 and dumps a per-switch RIB section.
 */
TEST_F(BgpSimulatorCliTest, RunSimulationDumpsRib) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");

  std::ostringstream oss;
  const int rc = runSimulation({configPath}, oss);

  EXPECT_EQ(0, rc);
  const std::string out = oss.str();
  EXPECT_NE(std::string::npos, out.find("Loaded 1 switch"));
  EXPECT_NE(std::string::npos, out.find("; converged after"));
  EXPECT_NE(std::string::npos, out.find("=== Switch stand_alone_conf ==="));
}

/*
 * A directory argument expands to the config files it contains.
 */
TEST_F(BgpSimulatorCliTest, CollectConfigPathsExpandsDirectory) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");
  const auto dir = std::filesystem::path(configPath).parent_path().string();

  const auto paths = collectConfigPaths({dir});

  EXPECT_FALSE(paths.empty());
  const bool foundSampleConfig =
      std::any_of(paths.begin(), paths.end(), [](const std::string& p) {
        return p.find("stand_alone_conf.json") != std::string::npos;
      });
  EXPECT_TRUE(foundSampleConfig);
  EXPECT_TRUE(std::is_sorted(paths.begin(), paths.end()));
}

/*
 * The same config passed more than once is collapsed to a single entry so the
 * simulator does not build duplicate switches.
 */
TEST_F(BgpSimulatorCliTest, CollectConfigPathsDeduplicates) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");

  const auto paths = collectConfigPaths({configPath, configPath});

  const std::vector<std::string> expected{configPath};
  EXPECT_EQ(expected, paths);
}

/*
 * File arguments given out of order are returned globally sorted, not in
 * command-line order.
 */
TEST_F(BgpSimulatorCliTest, CollectConfigPathsSortsArguments) {
  const std::vector<std::string> args{"b.json", "c.json", "a.json"};

  const std::vector<std::string> expected{"a.json", "b.json", "c.json"};
  EXPECT_EQ(expected, collectConfigPaths(args));
}

/*
 * A non-existent config file is reported as an error (non-zero exit).
 */
TEST_F(BgpSimulatorCliTest, NonExistentConfigReturnsError) {
  std::ostringstream oss;
  const int rc = runSimulation({"/tmp/bgp_sim_does_not_exist.json"}, oss);

  EXPECT_EQ(1, rc);
  EXPECT_NE(std::string::npos, oss.str().find("Error"));
}

} // namespace facebook::bgp
