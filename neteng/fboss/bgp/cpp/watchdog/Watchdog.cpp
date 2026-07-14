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

#include "neteng/fboss/bgp/cpp/watchdog/Watchdog.h"
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/logging/xlog.h>
#include "neteng/fboss/bgp/cpp/watchdog/MemProfiler.h"
#include "neteng/fboss/bgp/cpp/watchdog/QueryTree.h"

namespace facebook::bgp {

Watchdog::Watchdog(
    std::shared_ptr<const Config> config,
    SystemResourceLimits resourceLimits)
    : BgpModuleBase(kModuleWatchdog),
      config_(std::move(config)),
      resourceLimits_(std::move(resourceLimits)),
      startTime_(std::chrono::steady_clock::now()) {}

void Watchdog::run() noexcept {
  // periodically monitor system metrics.
  asyncScope_.add(co_withExecutor(&evb_, monitorSystemMetricsLoop()));

  // periodically snapshot heartbeat counters for stall detection.
  asyncScope_.add(co_withExecutor(&evb_, snapshotHeartbeatsLoop()));

  auto memConfig = config_->getMemoryProfilingConfig();
  if (memConfig && *memConfig->enable_memory_profiling()) {
    // enable heap profiling
    setHeapProfilingMode(true);

    // kick off coro task to do periodic dump
    asyncScope_.add(co_withExecutor(
        &evb_, dumpHeapProfileLoop(*memConfig->heap_dump_interval_s())));
  } else {
    setHeapProfilingMode(false);
  }

  // pump up the eventbase to run all coro tasks.
  evb_.loopForever();
}

void Watchdog::stop() noexcept {
  // cancel all coroutines
  folly::coro::blockingWait(asyncScope_.cancelAndJoinAsync());

  // terminate evb to deterministically shutdown evb
  evb_.terminateLoopSoon();
}

void Watchdog::monitorModule(
    const std::string& moduleName,
    MonitoredModule& module) noexcept {
  if (!module.isMonitored() &&
      monitoredModules_.find(moduleName) == monitoredModules_.end()) {
    monitoredModules_.emplace(moduleName, std::ref(module));
    module.markMonitored();
  } else {
    XLOGF(INFO, "Module {} is already monitored.", moduleName);
  }
}

/**
 * Get the queue sizes for the modules by querying the watchdog.
 *
 * @param paths The list of query paths to query. The path can contain
 * the module name or module name followed by queue name, e.g.
 * ["module1", "module2.queue1", "module3.queue2.queue3"]
 * @return The map of queue sizes where the key is the queue path and
 * the value is the size of the queue.
 */
QueueSizeMapT Watchdog::getQueueSizes(
    const std::unique_ptr<MonitoredPathT>& paths) noexcept {
  QueueSizeMapT queueSizes;

  // Build query tree
  QueryTree queryTree;
  for (const auto& path : *paths) {
    queryTree.addPath(path);
  }
  if (paths->empty()) {
    queryTree.root.markLeaf();
  }

  for (const auto& [moduleName, module] : monitoredModules_) {
    auto modulePreifx = moduleName + ".";

    auto queryNode = &queryTree.root;
    if (!queryNode->isLeaf) {
      if (!queryNode->children.contains(moduleName)) {
        // skip if module is not in query tree
        continue;
      }
      queryNode = queryNode->children.at(moduleName).get();
    }

    auto moduleQueueSizes = module.get().getQueueSizes(queryNode);
    for (const auto& [queueName, queueSize] : moduleQueueSizes) {
      queueSizes.emplace(modulePreifx + queueName, queueSize);
    }
  }
  return queueSizes;
}

folly::coro::Task<void> Watchdog::monitorSystemMetricsLoop() {
  XLOG(INFO, "Starting system metrics monitoring coro task");

  while (true) {
    // when cancelAndJoinAsync is called, guaranteed to exit
    co_await folly::coro::co_safe_point;

    // sleep before next metrics update
    co_await folly::coro::sleep(kWatchdogSystemMetricsInterval);

    // Update system metrics
    updateSystemMetrics();
  }

  XLOG(INFO, "[Exit] Successfully stopped system metrics monitoring task");
}

int64_t Watchdog::getUptimeSeconds() const {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::steady_clock::now() - startTime_)
      .count();
}

void Watchdog::updateSystemMetrics() {
  // Update uptime counter
  const std::chrono::steady_clock::time_point now =
      std::chrono::steady_clock::now();
  const int64_t uptimeSeconds = getUptimeSeconds();

  fb303::ThreadCachedServiceData::get()->setCounter(
      "bgpd.process.uptime.seconds", uptimeSeconds);

  // Update memory RSS counter
  const std::optional<size_t> rssMem = systemMetrics_.getRSSMemBytes();
  if (rssMem.has_value()) {
    fb303::ThreadCachedServiceData::get()->setCounter(
        "bgpd.process.memory.rss.bytes", static_cast<int64_t>(*rssMem));
  }

  // Update CPU percentage counter
  const std::optional<double> cpuPct = systemMetrics_.getCPUpercentage();
  if (cpuPct.has_value()) {
    const int64_t cpuPctInt = static_cast<int64_t>(*cpuPct);
    fb303::ThreadCachedServiceData::get()->setCounter(
        "bgpd.process.cpu.percent", cpuPctInt);
  }

  // Log periodic summary at INFO level (less frequent than individual metrics)
  std::chrono::steady_clock::time_point lastSummaryTime =
      std::chrono::steady_clock::now();
  const std::chrono::minutes kSummaryInterval = std::chrono::minutes(5);

  if (now - lastSummaryTime >= kSummaryInterval) {
    XLOGF(
        INFO,
        "BGP++ System Metrics - Uptime: {}s, RSS: {} bytes, CPU: {}%",
        uptimeSeconds,
        rssMem.value_or(0),
        cpuPct.value_or(0.0));
    lastSummaryTime = now;
  }
}

folly::coro::Task<void> Watchdog::snapshotHeartbeatsLoop() {
  XLOG(INFO, "Starting heartbeat snapshot coro task");

  static const std::vector<std::string> kHeartbeatModules = {
      kModuleRib,
      kModulePeerManager,
      kModuleSessionManager,
      kModuleNeighborWatcher,
      kModuleNexthopHandler,
      kModuleNetlinkWrapper,
      kModuleWatchdog,
  };

  while (true) {
    co_await folly::coro::co_safe_point;
    co_await folly::coro::sleepReturnEarlyOnCancel(
        kWatchdogHeartbeatSnapshotInterval);

    try {
      auto now = std::chrono::duration_cast<std::chrono::seconds>(
                     std::chrono::steady_clock::now().time_since_epoch())
                     .count();

      folly::F14FastMap<std::string, HeartbeatSnapshot> localSnapshots;
      for (const auto& moduleName : kHeartbeatModules) {
        auto counterKey = fmt::format("bgpd.heartbeat.{}", moduleName);
        auto optVal = fb303::ThreadCachedServiceData::get()
                          ->getServiceData()
                          ->getCounterIfExists(counterKey);
        if (optVal.has_value()) {
          localSnapshots[moduleName] = HeartbeatSnapshot{now, *optVal};
        }
      }

      auto snapshots = heartbeatSnapshots_.wlock();
      for (auto& [moduleName, snap] : localSnapshots) {
        auto& deq = (*snapshots)[moduleName];
        deq.push_back(std::move(snap));
        while (deq.size() > kMaxHeartbeatSnapshots) {
          deq.pop_front();
        }
      }
    } catch (const std::exception& ex) {
      XLOGF(ERR, "Heartbeat snapshot failed: {}", ex.what());
    }
  }

  XLOG(INFO, "[Exit] Successfully stopped heartbeat snapshot task");
}

folly::F14FastMap<std::string, std::deque<HeartbeatSnapshot>>
Watchdog::getHeartbeatSnapshots() const {
  return *heartbeatSnapshots_.rlock();
}

folly::coro::Task<void> Watchdog::dumpHeapProfileLoop(
    const int32_t intervalInSecond) {
  XLOGF(
      INFO,
      "Starting heap dump profiling coro task with {}s interval",
      intervalInSecond);

  while (true) {
    // when cancelAndJoinAsync is called, guaranteed to exit
    co_await folly::coro::co_safe_point;

    co_await folly::coro::sleep(std::chrono::seconds(intervalInSecond));

    // Dump heap profile periodically
    getHeapDump("bgpcpp");
  }

  XLOG(INFO, "[Exit] Successfully stopped heap dump profiling task");
}

} // namespace facebook::bgp
