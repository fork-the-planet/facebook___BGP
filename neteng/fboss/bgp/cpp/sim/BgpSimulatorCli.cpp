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

#include "neteng/fboss/bgp/cpp/sim/BgpSimulatorCli.h"

#include <algorithm>
#include <filesystem>
#include <ostream>
#include <unordered_set>

#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/sim/BgpSimulator.h"
#include "neteng/fboss/bgp/cpp/sim/BgpSwitch.h"

namespace facebook::bgp {

std::vector<std::string> collectConfigPaths(
    const std::vector<std::string>& args) {
  namespace fs = std::filesystem;
  std::vector<std::string> paths;
  for (const auto& arg : args) {
    const fs::path path(arg);
    /*
     * Use the std::error_code overloads so a filesystem error (e.g. a
     * permission-denied directory or an entry that disappears mid-iteration)
     * degrades gracefully instead of throwing an uncaught filesystem_error out
     * of main(). A path that is not a readable directory is treated as an
     * explicit file argument and surfaced later by loadConfigs.
     */
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
      std::vector<std::string> dirFiles;
      for (fs::directory_iterator it(path, ec), end; !ec && it != end;
           it.increment(ec)) {
        /*
         * Only pick up .json config files so a directory that also holds
         * non-config artifacts (README, notes, etc.) still runs cleanly.
         * Use a separate error_code so a transient stat error on one entry
         * does not poison the loop's ec and prematurely end the scan.
         */
        std::error_code fileEc;
        if (it->is_regular_file(fileEc) && it->path().extension() == ".json") {
          dirFiles.push_back(it->path().string());
        }
      }
      if (dirFiles.empty()) {
        XLOGF(WARN, "Directory {} contained no .json config files", arg);
      }
      paths.insert(paths.end(), dirFiles.begin(), dirFiles.end());
    } else {
      paths.push_back(arg);
    }
  }
  /*
   * De-duplicate so the same config passed twice (e.g. explicitly and again
   * via its directory, or simply listed twice) does not build duplicate
   * switches and trip the name/address collision warnings in BgpSimulator.
   * Comparison is lexical, so distinct spellings of the same file (./a.json vs
   * a.json) are not collapsed.
   */
  std::vector<std::string> deduped;
  deduped.reserve(paths.size());
  std::unordered_set<std::string> seen;
  for (auto& path : paths) {
    if (seen.insert(path).second) {
      deduped.push_back(std::move(path));
    }
  }
  /*
   * Globally sort the de-duplicated result so the returned list is in a single
   * deterministic order regardless of the order arguments were given on the
   * command line. Comparison is lexical, so distinct spellings of the same file
   * (./a.json vs a.json) sort independently.
   */
  std::sort(deduped.begin(), deduped.end());
  return deduped;
}

int runSimulation(
    const std::vector<std::string>& configPaths,
    std::ostream& os) {
  BgpSimulator simulator;
  try {
    simulator.loadConfigs(configPaths);
    simulator.resolvePeerLinks();
    const size_t iterations = simulator.run();
    const bool converged = iterations < BgpSimulator::kDefaultMaxIterations;
    os << "Loaded " << simulator.numSwitches() << " switch(es); "
       << (converged ? "converged" : "did NOT converge") << " after "
       << iterations << " iteration(s)\n\n";
    for (const auto& sw : simulator.switches()) {
      os << "=== Switch " << sw->name() << " ===\n"
         << sw->routingTable().toDebugString() << "\n";
    }
  } catch (const std::exception& ex) {
    os << "Error: " << ex.what() << "\n";
    return 1;
  }
  return 0;
}

} // namespace facebook::bgp
