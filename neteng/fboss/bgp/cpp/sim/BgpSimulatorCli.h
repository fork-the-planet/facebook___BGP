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

#include <iosfwd>
#include <string>
#include <vector>

namespace facebook::bgp {

/*
 * Expand CLI arguments into a sorted list of per-switch config file paths.
 * Directory arguments are expanded to the regular files they contain; file
 * arguments are passed through.
 */
std::vector<std::string> collectConfigPaths(
    const std::vector<std::string>& args);

/*
 * Load the given per-switch configs, resolve peer links, run the simulation to
 * convergence, and dump each switch's RIB to `os`. Returns 0 on success or 1 on
 * error (the error is also written to `os`).
 */
int runSimulation(
    const std::vector<std::string>& configPaths,
    std::ostream& os);

} // namespace facebook::bgp
