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

#include "neteng/fboss/bgp/cpp/sim/BgpSimulator.h"

#include <filesystem>
#include <utility>

#include <folly/container/F14Map.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/sim/BgpSwitch.h"

namespace facebook::bgp {

BgpSimulator::~BgpSimulator() {
  /*
   * Peers hold shared_ptr references to their remote switches, so reciprocal
   * links form shared_ptr cycles. Break them here so the switches are freed
   * when the simulator is destroyed.
   */
  for (auto& sw : switches_) {
    for (auto& peer : sw->peers()) {
      peer.setRemoteSwitch(nullptr);
    }
  }
}

void BgpSimulator::loadConfigs(const std::vector<std::string>& configPaths) {
  /*
   * Switch names are derived from the config file stem, which is not guaranteed
   * unique across paths (e.g. dirA/rsw001.json and dirB/rsw001.json both yield
   * "rsw001"). Duplicate names make downstream collision warnings (which print
   * sw->name()) ambiguous, so warn here with the conflicting paths.
   */
  folly::F14FastMap<std::string, std::string> nameToPath;
  for (const auto& path : configPaths) {
    /*
     * populateConfigDb=false: we only need the deserialized thrift struct, not
     * the production Config runtime (FeatureFlags / netwhoami / fb303).
     */
    Config config(
        path, /*peerSubnetLbwMap=*/std::nullopt, /*populateConfigDb=*/false);
    auto name = std::filesystem::path(path).stem().string();
    auto [it, inserted] = nameToPath.emplace(name, path);
    if (!inserted) {
      XLOGF(
          WARN,
          "BgpSimulator: switch name {} derived from config {} duplicates the "
          "name already loaded from config {}; switch names will be ambiguous "
          "in peer-link resolution and collision warnings",
          name,
          path,
          it->second);
    }
    switches_.push_back(
        std::make_shared<BgpSwitch>(std::move(name), config.getConfig()));
  }
}

void BgpSimulator::addSwitch(std::shared_ptr<BgpSwitch> bgpSwitch) {
  switches_.push_back(std::move(bgpSwitch));
}

folly::F14FastMap<folly::IPAddress, std::shared_ptr<BgpSwitch>>
BgpSimulator::buildAddrToSwitchMap() const {
  folly::F14FastMap<folly::IPAddress, std::shared_ptr<BgpSwitch>> addrToSwitch;
  for (const auto& sw : switches_) {
    if (sw->routerId() != 0) {
      const auto addr = folly::IPAddress(
          folly::IPAddressV4::fromLongHBO(
              static_cast<uint32_t>(sw->routerId())));
      auto [it, inserted] = addrToSwitch.emplace(addr, sw);
      if (!inserted) {
        XLOGF(
            WARN,
            "BgpSimulator: address {} claimed by switch {} collides with "
            "switch {}; peer links to this address will resolve to the first",
            addr.str(),
            sw->name(),
            it->second->name());
      }
    }
    for (const auto& peer : sw->peers()) {
      const auto& localIp = peer.localIp();
      if (localIp.isV4() || localIp.isV6()) {
        auto [it, inserted] = addrToSwitch.emplace(localIp, sw);
        if (!inserted && it->second != sw) {
          XLOGF(
              WARN,
              "BgpSimulator: local address {} on switch {} collides with "
              "switch {}; peer links to this address will resolve to the first",
              localIp.str(),
              sw->name(),
              it->second->name());
        }
      }
    }
  }
  return addrToSwitch;
}

void BgpSimulator::resolvePeerLinks() {
  const auto addrToSwitch = buildAddrToSwitchMap();
  for (auto& sw : switches_) {
    for (auto& peer : sw->peers()) {
      const auto it = addrToSwitch.find(peer.peerIp());
      if (it == addrToSwitch.end()) {
        XLOGF(
            DBG2,
            "BgpSimulator: peer {} (remote-AS {}) on switch {} is unresolved "
            "(no modeled switch owns that address); leaving unlinked",
            peer.peerIp().str(),
            peer.remoteAsn(),
            sw->name());
        continue;
      }
      if (it->second == sw) {
        /*
         * The peer address resolves to the peer's own switch (e.g. it matches
         * the switch's router-id or a local address). A switch peering with
         * itself is invalid in BGP, so skip the self-link.
         */
        XLOGF(
            WARN,
            "BgpSimulator: peer {} (remote-AS {}) on switch {} resolves to its "
            "own switch; skipping self-link",
            peer.peerIp().str(),
            peer.remoteAsn(),
            sw->name());
        continue;
      }
      peer.setRemoteSwitch(it->second);
    }
  }
}

size_t BgpSimulator::run(size_t maxIterations) {
  for (auto& sw : switches_) {
    sw->originateRoutes();
  }
  /*
   * Select best paths for the freshly originated routes before the first
   * propagate pass: origination inserts paths but does not run selection, so
   * without this the first iteration would propagate nothing and the returned
   * iteration count would overstate the true convergence depth by one.
   */
  for (auto& sw : switches_) {
    sw->runBestPathSelection();
  }

  size_t iterations = 0;
  while (iterations < maxIterations) {
    ++iterations;
    for (auto& sw : switches_) {
      sw->propagateRoutes();
    }
    bool changed = false;
    for (auto& sw : switches_) {
      if (sw->runBestPathSelection()) {
        changed = true;
      }
    }
    if (!changed) {
      return iterations; // converged
    }
  }

  XLOGF(
      WARN,
      "BgpSimulator: did not converge after {} iterations",
      maxIterations);
  return iterations;
}

} // namespace facebook::bgp
