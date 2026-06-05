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

#include <iostream>
#include <string>
#include <vector>

#include <folly/init/Init.h>

#include "neteng/fboss/bgp/cpp/sim/BgpSimulatorCli.h"

int main(int argc, char* argv[]) {
  folly::Init init(&argc, &argv);

  const std::vector<std::string> args(argv + 1, argv + argc);
  const auto configPaths = facebook::bgp::collectConfigPaths(args);
  if (configPaths.empty()) {
    std::cerr << "Usage: " << argv[0] << " <config_dir | config_file>..."
              << std::endl;
    return 1;
  }

  return facebook::bgp::runSimulation(configPaths, std::cout);
}
