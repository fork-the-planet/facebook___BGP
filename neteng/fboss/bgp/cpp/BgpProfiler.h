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
 * BgpProfiler - Lightweight coroutine profiler for BGP++
 *
 * Design:
 * - Disabled by default (controlled via gflag or Thrift)
 * - Ultra-low overhead (~100-150ns per call when enabled)
 * - Runtime kill switch via Thrift
 * - ODS export for EBB platform
 *
 * Usage:
 * // For coroutines - wrap at call site
 * co_await profiledTask("AdjRib::processPeerUpdate",
 * processPeerUpdate(update));
 *
 * // For sync functions - RAII at top
 * ScopedProfile p("PeerManagerBase::processPeerEvent");
 */

#pragma once

#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/coro/Task.h>
#include <folly/stats/Histogram.h>
#include <gflags/gflags.h>

#include <re2/re2.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

DECLARE_bool(bgp_coro_profiler_enabled);
DECLARE_bool(bgp_coro_profiler_export_ods);

namespace facebook::bgp {

/*
 * Thread identifier for lock-free profiling
 * BGP++ has 2 main threads: RIB and PeerManagerBase
 */
enum class ProfilerThread {
  RIB,
  PEER_MANAGER,
};

/*
 * Per-function statistics
 * Lock-free design: each thread writes to its own histogram
 */
struct CoroStat {
  std::atomic<uint64_t> count{0};

  struct ThreadData {
    folly::Histogram<int64_t> histogram;
    int64_t maxUs{0};
    int64_t totalUs{0};
    ThreadData();
  };

  ThreadData ribThreadData;
  ThreadData peerMgrThreadData;

  CoroStat();

  ThreadData& getThreadData(ProfilerThread thread) {
    switch (thread) {
      case ProfilerThread::RIB:
        return ribThreadData;
      case ProfilerThread::PEER_MANAGER:
        return peerMgrThreadData;
      default:
        return ribThreadData;
    }
  }

  folly::Histogram<int64_t> getMergedHistogram() const;
  int64_t getMergedMaxUs() const;
  int64_t getMergedTotalUs() const;
};

/*
 * Singleton profiler for BGP coroutines
 */
class BgpProfiler {
 public:
  static BgpProfiler* getInstance();

  /*
   * Control - gflag at startup, Thrift at runtime
   */
  void setEnabled(bool enabled);
  bool isEnabled() const {
    return enabled_.load(std::memory_order_relaxed);
  }

  /*
   * Optional regex filter to limit which functions are profiled.
   * Uses pre-computed matching for O(1) hot path lookups.
   */
  void setFilterRegex(const std::string& regexStr);
  bool matchesFilter(const std::string& name);
  bool hasFilter() const {
    return hasFilter_.load(std::memory_order_relaxed);
  }

  /*
   * Thread registration for lock-free profiling
   */
  void setCurrentThread(ProfilerThread thread);
  ProfilerThread getCurrentThread() const;

  /*
   * Recording - called by ScopedProfile and profiledTask
   */
  void recordFinish(const std::string& name, std::chrono::nanoseconds duration);
  void recordError(const std::string& name, std::chrono::nanoseconds duration);

  /*
   * Stats retrieval
   */
  struct StatSummary {
    std::string name;
    uint64_t count;
    int64_t p50Ms;
    int64_t p90Ms;
    int64_t p99Ms;
    int64_t maxMs;
    int64_t totalMs;
  };

  std::vector<StatSummary> getStats() const;
  void clearStats();
  void dumpStatsToFile(const std::string& path) const;

  /*
   * Write profiler stats to fb303 counters.
   * Controlled by --bgp_coro_profiler_export_ods gflag.
   */
  void writeToFb303();

 private:
  BgpProfiler() = default;

  std::shared_ptr<CoroStat> getStat(const std::string& name);

  std::atomic<bool> enabled_{false};
  std::atomic<bool> hasFilter_{false};
  folly::Synchronized<std::unique_ptr<re2::RE2>> filterRegex_;
  folly::Synchronized<folly::F14FastMap<std::string, std::shared_ptr<CoroStat>>>
      stats_;

  /*
   * Pre-computed filter matches for O(1) hot path lookups.
   * - matchedNames_: functions that match the current filter
   * - checkedNames_: functions already checked against regex (superset of
   * matched) Both are cleared when filter changes.
   */
  folly::Synchronized<folly::F14FastSet<std::string>> matchedNames_;
  folly::Synchronized<folly::F14FastSet<std::string>> checkedNames_;
};

/*
 * RAII helper for profiling synchronous functions
 *
 * Usage:
 *   void someFunction() {
 *     ScopedProfile p("ClassName::someFunction");
 *     // ... function body
 *   }
 */
class ScopedProfile {
 public:
  explicit ScopedProfile(const std::string& name);
  ~ScopedProfile();

  /* Disable copy/move to prevent accidental double-recording */
  ScopedProfile(const ScopedProfile&) = delete;
  ScopedProfile& operator=(const ScopedProfile&) = delete;
  ScopedProfile(ScopedProfile&&) = delete;
  ScopedProfile& operator=(ScopedProfile&&) = delete;

 private:
  std::string name_;
  std::chrono::steady_clock::time_point start_;
  bool shouldRecord_{false};
};

/*
 * Template wrapper for profiling coroutines
 *
 * Usage:
 *   co_await profiledTask("AdjRib::processPeerUpdate",
 * processPeerUpdate(update));
 */
template <typename T>
folly::coro::Task<T> profiledTask(
    const std::string& name,
    folly::coro::Task<T> innerTask) {
  bool shouldProfile = false;
  try {
    auto* profiler = BgpProfiler::getInstance();
    shouldProfile = profiler->isEnabled() &&
        (!profiler->hasFilter() || profiler->matchesFilter(name));
  } catch (...) {
    // Profiler must never crash the caller
  }

  if (!shouldProfile) {
    co_return co_await std::move(innerTask);
  }

  auto start = std::chrono::steady_clock::now();
  T result = co_await std::move(innerTask);
  try {
    auto end = std::chrono::steady_clock::now();
    BgpProfiler::getInstance()->recordFinish(name, end - start);
  } catch (...) {
    // Profiler must never crash the caller
  }
  co_return result;
}

/*
 * Specialization for void tasks
 */
inline folly::coro::Task<void> profiledTask(
    const std::string& name,
    folly::coro::Task<void> innerTask) {
  bool shouldProfile = false;
  try {
    auto* profiler = BgpProfiler::getInstance();
    shouldProfile = profiler->isEnabled() &&
        (!profiler->hasFilter() || profiler->matchesFilter(name));
  } catch (...) {
    // Profiler must never crash the caller
  }

  if (!shouldProfile) {
    co_await std::move(innerTask);
    co_return;
  }

  auto start = std::chrono::steady_clock::now();
  co_await std::move(innerTask);
  try {
    auto end = std::chrono::steady_clock::now();
    BgpProfiler::getInstance()->recordFinish(name, end - start);
  } catch (...) {
    // Profiler must never crash the caller
  }
}

} // namespace facebook::bgp
