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

#include "neteng/fboss/bgp/cpp/nexthopTracker/FsdbFibWatcher.h"
#include <fboss/agent/if/gen-cpp2/FbossCtrlAsyncClient.h>
#include <fboss/agent/if/gen-cpp2/common_types.h>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/ThriftClientUtils.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"
namespace facebook::bgp {
namespace thrift_tags = apache::thrift::ident;

FsdbFibWatcher::FsdbFibWatcher(
    std::shared_ptr<NexthopCache> nexthopCache,
    nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage>& ribInQ,
    folly::EventBase* evb,
    std::shared_ptr<fboss::fsdb::FsdbCowStateSubManager> fsdbSubMgr,
    std::optional<fboss::ClientID> igpCostClientId)
    : fsdbSubMgr_(std::move(fsdbSubMgr)),
      nexthopCache_(std::move(nexthopCache)),
      ribInQ_(ribInQ),
      evb_(evb),
      igpCostClientId_(igpCostClientId) {}

void FsdbFibWatcher::addPeerAddress(const folly::IPAddress& peerAddr) {
  if (pathsAdded_) {
    XLOGF(
        WARNING,
        "[FsdbFibWatcher] Cannot add peer {} after paths have been registered"
        " — runtime subscription changes are not yet supported",
        peerAddr.str());
    return;
  }
  if (subscribedPeers_.insert(peerAddr).second) {
    XLOGF(INFO, "[FsdbFibWatcher] Added peer address: {}", peerAddr.str());
  }
}

void FsdbFibWatcher::removePeerAddress(const folly::IPAddress& peerAddr) {
  if (pathsAdded_) {
    XLOGF(
        WARNING,
        "[FsdbFibWatcher] Cannot remove peer {} after paths have been"
        " registered — runtime subscription changes are not yet supported",
        peerAddr.str());
    return;
  }
  if (subscribedPeers_.erase(peerAddr)) {
    XLOGF(INFO, "[FsdbFibWatcher] Removed peer address: {}", peerAddr.str());
  }
}

void FsdbFibWatcher::ensureSwitchIds() noexcept {
  if (!switchIds_.empty()) {
    return;
  }
  /**
   * Fetch switch IDs from the FBOSS agent so we subscribe to FIB host
   * routes under every switch ID on the device, not just "0".
   **/
  try {
    folly::EventBase evb;
    auto client =
        createThriftClient<apache::thrift::Client<facebook::fboss::FbossCtrl>>(
            evb,
            kLoopBackAddressV6,
            kFbossAgentPort,
            kFbossAgentConnTimeout,
            kFbossAgentSendTimeout,
            kFbossAgentRecvTimeout);
    std::map<int64_t, facebook::fboss::cfg::SwitchInfo> switchInfoMap;
    client->sync_getSwitchIdToSwitchInfo(switchInfoMap);
    for (const auto& [switchId, _] : switchInfoMap) {
      switchIds_.push_back(fmt::format("id={}", switchId));
    }
    XLOGF(
        INFO,
        "[FsdbFibWatcher] Retrieved {} switch IDs from agent: [{}]",
        switchIds_.size(),
        fmt::join(switchIds_, ", "));
  } catch (const std::exception& ex) {
    XLOGF(
        WARNING,
        "[FsdbFibWatcher] Failed to get switch IDs from agent: {}."
        " Defaulting to \"0\"",
        ex.what());
    switchIds_.emplace_back("id=0");
  }
}

void FsdbFibWatcher::addFsdbPathsForNexthop(const folly::IPAddress& nexthop) {
  bool isV4 = nexthop.isV4();
  auto hostPrefix = fmt::format("{}/{}", nexthop.str(), isV4 ? 32 : 128);
  for (const auto& switchId : switchIds_) {
    /*
     * Idempotent guard: skip a (switchId, prefix) path already registered with
     * the sub manager. Without this, a retry after a partial failure (see
     * addNexthopPaths) would re-add an already-registered path and trip the sub
     * manager's fatal "Duplicate path added" CHECK. The key is recorded only
     * after addPath() succeeds, so a throwing addPath leaves the path eligible
     * for a clean retry.
     */
    auto pathKey = fmt::format("{}|{}", switchId, hostPrefix);
    if (registeredFsdbPaths_.contains(pathKey)) {
      continue;
    }
    auto basePath = fsdbStateRootPath_.agent()
                        .switchState()
                        .fibsInfoMap()[switchId]
                        .fibsMap()[kDefaultVrfId];
    if (isV4) {
      fsdbSubMgr_->addPath(basePath.fibV4()[hostPrefix]);
    } else {
      fsdbSubMgr_->addPath(basePath.fibV6()[hostPrefix]);
    }
    registeredFsdbPaths_.insert(pathKey);
    auto pathTokens = isV4 ? basePath.fibV4()[hostPrefix].tokens()
                           : basePath.fibV6()[hostPrefix].tokens();
    XLOGF(
        INFO,
        "[FsdbFibWatcher] Added FSDB path: [{}]",
        fmt::join(pathTokens, "/"));
  }
  subscribedPrefixes_.emplace(hostPrefix, std::make_pair(nexthop, isV4));
}

void FsdbFibWatcher::addPaths() noexcept {
  ensureSwitchIds();
  size_t failed = 0;
  for (const auto& peerAddr : subscribedPeers_) {
    /*
     * Best-effort: addFsdbPathsForNexthop() can throw (e.g. allocation
     * failure). This method is noexcept, so an escaping exception would
     * std::terminate the process — catch per-nexthop, log, and keep going so a
     * single failure cannot crash the daemon nor block the remaining paths.
     */
    try {
      addFsdbPathsForNexthop(peerAddr);
    } catch (const std::exception& ex) {
      ++failed;
      XLOGF(
          ERR,
          "[FsdbFibWatcher] Failed to add FSDB paths for {}: {}",
          peerAddr.str(),
          ex.what());
    }
  }
  pathsAdded_ = true;

  XLOGF(
      INFO,
      "[FsdbFibWatcher] Added FSDB paths for {} nexthops x {} switch IDs"
      " ({} nexthop(s) failed)",
      subscribedPeers_.size(),
      switchIds_.size(),
      failed);
}

std::vector<folly::IPAddress> FsdbFibWatcher::filterNewNexthops(
    const std::vector<folly::IPAddress>& nexthops) const {
  std::vector<folly::IPAddress> newNexthops;
  folly::F14FastSet<folly::IPAddress> seen;
  for (const auto& nh : nexthops) {
    // Skip nexthops already tracked, and de-dupe within the input.
    if (subscribedPeers_.count(nh) == 0 && seen.insert(nh).second) {
      newNexthops.push_back(nh);
    }
  }
  return newNexthops;
}

void FsdbFibWatcher::addNexthopPaths(
    const std::vector<folly::IPAddress>& newNexthops) {
  /**
   * Register FSDB FIB host-route paths for nexthops learned at runtime (e.g.,
   * from RIB-IN). The caller MUST stop the shared subscription first (addPath
   * asserts there is no live subscriber) and re-subscribe the full path set
   * afterwards.
   **/
  ensureSwitchIds();
  size_t added = 0;
  for (const auto& nh : newNexthops) {
    if (subscribedPeers_.count(nh) != 0) {
      // Already tracked — paths already registered.
      continue;
    }
    /*
     * Register paths first, then commit to subscribedPeers_ only on success. If
     * addFsdbPathsForNexthop throws, the nexthop is left OUT of
     * subscribedPeers_ so filterNewNexthops will surface it again on a future
     * request (retry) rather than silently skipping it forever.
     * addFsdbPathsForNexthop is idempotent, so the retry re-adds only the paths
     * not registered on the failed attempt. The exception is swallowed (logged)
     * so it can neither crash the process nor abort the remaining nexthops /
     * the re-subscribe.
     */
    try {
      addFsdbPathsForNexthop(nh);
      subscribedPeers_.insert(nh);
      ++added;
    } catch (const std::exception& ex) {
      XLOGF(
          ERR,
          "[FsdbFibWatcher] Failed to add FSDB paths for nexthop {}: {}"
          " — will retry on a future request",
          nh.str(),
          ex.what());
    }
  }
  if (added > 0) {
    XLOGF(
        INFO,
        "[FsdbFibWatcher] Added {} new nexthop(s) for tracking ({} total)",
        added,
        subscribedPeers_.size());
  }
}

void FsdbFibWatcher::registerPeers(
    const std::vector<folly::IPAddress>& peerAddresses) {
  for (const auto& peerAddr : peerAddresses) {
    addPeerAddress(peerAddr);
  }

  evb_->runInEventBaseThread([this]() { addPaths(); });

  XLOGF(
      INFO,
      "[FsdbFibWatcher] {} static peer addresses queued for subscription",
      peerAddresses.size());
}

void FsdbFibWatcher::stop() noexcept {
  // Subscription lifecycle is managed by the shared FsdbCowStateSubManager
  // owner
}

folly::coro::Task<void> FsdbFibWatcher::co_updateCacheAndPushToRib(
    const std::vector<NexthopStatus>& updates) {
  auto statuses = nexthopCache_->addOrUpdateNextHopStatus(updates);
  if (!statuses.empty()) {
    auto result = co_await co_awaitTry(
        ribInQ_.push(RibInNexthopUpdate(std::move(statuses))));
    if (!result.hasValue()) {
      co_yield folly::coro::co_error(std::move(result).exception());
    }
  }
}

folly::coro::Task<void> FsdbFibWatcher::co_markNeedsReconcile() {
  if (reachableNexthops_.empty()) {
    XLOG(
        INFO,
        "[FsdbFibWatcher] Reconnect: no previously reachable nexthops to"
        " clear");
    co_return;
  }
  XLOGF(
      INFO,
      "[FsdbFibWatcher] Reconnect: clearing {} reachable nexthops —"
      " will rebuild from notifications",
      reachableNexthops_.size());
  FsdbStats::incrFsdbNhtDisconnects();
  FsdbStats::setFsdbNhtConnected(0);
  std::vector<NexthopStatus> batchUpdates;
  for (const auto& ip : reachableNexthops_) {
    batchUpdates.emplace_back(
        ip,
        /*isReachable=*/false,
        /*igpCost=*/std::nullopt,
        /*isConnected=*/std::nullopt,
        /*excludeNexthopWithoutCost=*/false);
  }
  reachableNexthops_.clear();

  auto result = co_await co_awaitTry(co_updateCacheAndPushToRib(batchUpdates));
  if (result.hasException<folly::OperationCancelled>()) {
    co_yield folly::coro::co_error(std::move(result).exception());
  }
  if (result.hasException()) {
    XLOGF(
        ERR,
        "[FsdbFibWatcher] co_markNeedsReconcile failed: {}",
        result.exception().what());
  }
}

std::optional<uint32_t> FsdbFibWatcher::lookupIgpCostFromRoute(
    const facebook::fboss::state::RouteFields& route,
    const std::optional<fboss::ClientID>& igpCostClientId) {
  /*
   * A FIB route node carries two distinct nexthop sets:
   *  - nexthopsmulti.client2NextHopEntry: ALL nexthops contributed per source
   *    protocol/client (e.g. Open/R, TE Agent, BGP). Each client's entry holds
   *    that protocol's own per-nexthop cost (its IGP metric).
   *  - fwd: the nexthops the agent actually selected for forwarding, after
   *    resolving/tie-breaking across all clients (lowest admin distance wins).
   *
   * When igpCostClientId is unset, the cost is the minimum over fwd.nexthops[]
   * (legacy behavior).
   *
   * When igpCostClientId is set, the IGP cost is taken SOLELY from that
   * client's entry: we read the configured client's (e.g. Open/R) nexthops and
   * return the minimum cost over them.
   *
   * NOTE -- no intersection with fwd. We intentionally do NOT intersect the
   * configured client's nexthops with the resolved fwd nexthops, and we do NOT
   * check whether the two sets overlap at all. When the flag is set we blindly
   * trust the configured IGP: the cost comes only from that client's nexthops,
   * regardless of which nexthops the agent ultimately selected for forwarding.
   * This is deliberate for the current limited single-IGP (Open/R) use-case --
   * the fwd nexthops are not consulted for cost, only for the legacy
   * (flag-unset) path above.
   *
   * If the client has no entry (or none of its nexthops carry a cost), the
   * cost is left unset (nullopt). The route still stays reachable; an unset
   * cost just means the path takes part in best-path selection without an
   * IGP-cost preference. During tie-breaking a path that has a cost is
   * preferred over one that does not (an unset cost is treated as the worst
   * possible cost, UINT32_MAX).
   */

  // Minimum cost over a nexthop list (nexthops without a cost are ignored).
  auto minCost = [](const std::vector<facebook::fboss::NextHopThrift>& nexthops)
      -> std::optional<uint32_t> {
    std::optional<uint32_t> result;
    for (const auto& nh : nexthops) {
      if (nh.cost().has_value()) {
        auto cost = static_cast<uint32_t>(*nh.cost());
        if (!result || cost < *result) {
          result = cost;
        }
      }
    }
    return result;
  };

  // No protocol configured: keep current behavior (min cost over fwd nexthops).
  if (!igpCostClientId.has_value()) {
    return minCost(*route.fwd()->nexthops());
  }

  /*
   * Protocol configured: read the IGP cost solely from that client's entry,
   * with no intersection against fwd (see NOTE above). Missing client entry =>
   * nullopt (route stays reachable, cost unset).
   */
  const auto& client2NextHopEntry =
      *route.nexthopsmulti()->client2NextHopEntry();
  auto it = client2NextHopEntry.find(*igpCostClientId);
  if (it == client2NextHopEntry.end()) {
    return std::nullopt;
  }
  return minCost(*it->second.nexthops());
}

std::pair<bool, std::optional<uint32_t>> FsdbFibWatcher::routeExistsInCowTree(
    const std::string& prefixStr,
    bool isV4,
    const fboss::fsdb::FsdbCowStateSubManager::Data& data) {
  auto agentNode = data->safe_cref<thrift_tags::agent>();
  if (!agentNode) {
    XLOGF(INFO, "[FsdbFibWatcher] routeExists({}): no agent node", prefixStr);
    return {false, std::nullopt};
  }
  auto switchStateNode =
      agentNode->template safe_cref<thrift_tags::switchState>();
  if (!switchStateNode) {
    XLOGF(
        INFO,
        "[FsdbFibWatcher] routeExists({}): no switchState node",
        prefixStr);
    return {false, std::nullopt};
  }
  auto fibsInfoMapNode =
      switchStateNode->template safe_cref<thrift_tags::fibsInfoMap>();
  if (!fibsInfoMapNode) {
    XLOGF(
        INFO,
        "[FsdbFibWatcher] routeExists({}): no fibsInfoMap node",
        prefixStr);
    return {false, std::nullopt};
  }

  for (const auto& switchId : switchIds_) {
    if (!fibsInfoMapNode->count(switchId)) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} not in fibsInfoMap",
          prefixStr,
          switchId);
      continue;
    }
    const auto& fibInfoNode = fibsInfoMapNode->cref(switchId);
    auto fibsMapNode = fibInfoNode->template safe_cref<thrift_tags::fibsMap>();
    if (!fibsMapNode) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} no fibsMap",
          prefixStr,
          switchId);
      continue;
    }
    if (!fibsMapNode->count(kDefaultVrfId)) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} vrfId=0 not in"
          " fibsMap",
          prefixStr,
          switchId);
      continue;
    }
    const auto& fibContainerNode = fibsMapNode->cref(kDefaultVrfId);
    auto mapNode = isV4
        ? fibContainerNode->template safe_cref<thrift_tags::fibV4>()
        : fibContainerNode->template safe_cref<thrift_tags::fibV6>();
    if (!mapNode) {
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): switchId={} no {} node",
          prefixStr,
          switchId,
          isV4 ? "fibV4" : "fibV6");
      continue;
    }
    if (mapNode->count(prefixStr) > 0) {
      const auto& routeNode = mapNode->cref(prefixStr);
      auto igpCost =
          lookupIgpCostFromRoute(routeNode->toThrift(), igpCostClientId_);
      XLOGF(
          INFO,
          "[FsdbFibWatcher] routeExists({}): found in switchId={} {}"
          " igpCost={}",
          prefixStr,
          switchId,
          isV4 ? "fibV4" : "fibV6",
          igpCost ? fmt::format("{}", *igpCost) : "none");
      return {true, igpCost};
    }
    XLOGF(
        INFO,
        "[FsdbFibWatcher] routeExists({}): prefix not in switchId={} {}",
        prefixStr,
        switchId,
        isV4 ? "fibV4" : "fibV6");
  }
  return {false, std::nullopt};
}

folly::coro::Task<void> FsdbFibWatcher::co_processFibUpdate(
    fboss::fsdb::FsdbCowStateSubManager::SubUpdate update) {
  /**
   * Purely event-driven: scan updatedPath tokens for subscribed prefix
   * strings. Struct fields use numeric thrift IDs, but map keys (like
   * "fdad:500::d:0/128") appear as literal string tokens. Match these
   * against subscribedPrefixes_ to find affected peers.
   *
   * No notification for a prefix means the route does not exist (peer is
   * unreachable). As notifications arrive, we update nexthop status.
   * On reconnect, markNeedsReconcile() clears all reachable state so
   * status is rebuilt purely from new notifications.
   **/
  FsdbStats::setFsdbNhtConnected(1);
  std::vector<NexthopStatus> batchUpdates;

  for (const auto& pathTokens : update.updatedPaths) {
    for (const auto& token : pathTokens) {
      auto it = subscribedPrefixes_.find(token);
      if (it == subscribedPrefixes_.end()) {
        continue;
      }
      const auto& [peerAddr, isV4] = it->second;
      const auto& prefixStr = it->first;
      auto [exists, igpCost] =
          routeExistsInCowTree(prefixStr, isV4, update.data);
      bool wasReachable = reachableNexthops_.contains(peerAddr);

      if (exists) {
        if (!wasReachable) {
          reachableNexthops_.insert(peerAddr);
          FsdbStats::incrFsdbNhtNexthopReachable();
        }
        batchUpdates.emplace_back(
            peerAddr,
            /*isReachable=*/true,
            igpCost,
            /*isConnected=*/std::nullopt,
            /*excludeNexthopWithoutCost=*/false);
        XLOGF(
            INFO,
            "[FsdbFibWatcher] Nexthop {}: {} igpCost={}",
            wasReachable ? "cost update" : "became reachable",
            peerAddr.str(),
            igpCost ? fmt::format("{}", *igpCost) : "none");
      } else {
        if (wasReachable) {
          reachableNexthops_.erase(peerAddr);
          FsdbStats::incrFsdbNhtNexthopUnreachable();
        }
        batchUpdates.emplace_back(
            peerAddr,
            /*isReachable=*/false,
            /*igpCost=*/std::nullopt,
            /*isConnected=*/std::nullopt,
            /*excludeNexthopWithoutCost=*/false);
        XLOGF(
            INFO,
            "[FsdbFibWatcher] Nexthop unreachable: {} igpCost=none",
            peerAddr.str());
      }
      break; // found the relevant token, done with this path
    }
  }

  XLOG(INFO, "batchUpdates", batchUpdates.size());
  if (!batchUpdates.empty()) {
    XLOGF(
        INFO,
        "[FsdbFibWatcher] Pushing {} nexthop updates to cache"
        " ({} total reachable)",
        batchUpdates.size(),
        reachableNexthops_.size());
    auto result =
        co_await co_awaitTry(co_updateCacheAndPushToRib(batchUpdates));
    if (result.hasException<folly::OperationCancelled>()) {
      co_yield folly::coro::co_error(std::move(result).exception());
    }
    if (result.hasException()) {
      XLOGF(
          ERR,
          "[FsdbFibWatcher] co_processFibUpdate failed: {}",
          result.exception().what());
    }
  }
}

} // namespace facebook::bgp
