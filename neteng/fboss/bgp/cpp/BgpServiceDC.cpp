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

#include <fb303/ServiceData.h>
#include <fboss/lib/LogThriftCall.h>
#include <folly/ExceptionString.h>
#include <folly/ScopeGuard.h>
#include <folly/futures/Future.h>
#include <folly/logging/xlog.h>
#include <neteng/fboss/bgp/cpp/BgpServiceDC.h>
#include <neteng/fboss/bgp/cpp/BgpServiceUtil.h>
#include <neteng/fboss/bgp/cpp/peer/PeerManager.h>
#include <neteng/fboss/bgp/cpp/rib/RibDC.h>
#include <neteng/fboss/bgp/cpp/rib/RibFileUtils.h>
#include <neteng/fboss/bgp/cpp/stats/Stats.h>
#include <neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h>
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/EvbUtils.h"

using namespace facebook::neteng::fboss::bgp_attr;
using namespace facebook::neteng::fboss::bgp::thrift;
using std::chrono::milliseconds;

namespace facebook::bgp {

DEFINE_int32(
    thrift_route_add_delay_ms,
    10,
    "Delay in msec between processing route adds from thrift calls (for testing only)");

static const std::string kExitNullPtrLogPrefix = "BgpServiceDCExitOrNullPtr";

BgpServiceDC::BgpServiceDC(
    PeerManager& peerMgr,
    std::shared_ptr<ConfigManager> configManager,
    RibDC& rib,
    std::shared_ptr<NeighborWatcher> neighborWatcher,
    Watchdog& watchdog,
    bool enable_thrift_protection)
    : BgpServiceBase(
          peerMgr,
          std::move(configManager),
          rib,
          watchdog,
          enable_thrift_protection),
      ribDC_(rib),
      neighborWatcher_(std::move(neighborWatcher)),
      dcRib_(rib) {}

folly::coro::Task<bool> BgpServiceDC::co_getIsSafeModeOn() {
  auto log = LOG_THRIFT_CALL(DBG2);
  co_return peerMgr_.getIsSafeModeOn();
}

int64_t BgpServiceDC::getGoldenVipsCount() {
  // Get the current RibPolicy instance.
  auto log = LOG_THRIFT_CALL(DBG2);
  return AdjRibPrefixSet::get()->goldenVipSize();
}

folly::coro::Task<std::unique_ptr<TGoldenPrefixesPolicyStatus>>
BgpServiceDC::co_getGoldenPrefixesPolicyStatus() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<TGoldenPrefixesPolicyStatus>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TGoldenPrefixesPolicyStatus>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto goldenPolicy = std::make_unique<TGoldenPrefixesPolicyStatus>();

  auto ribResult = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getRouteFilterPolicy(); },
      kRibThriftHandlerTimeout);

  if (ribResult.hasValue()) {
    goldenPolicy->policy() = std::move(ribResult.value());
  } else {
    if (ribResult.exception().is_compatible_with<folly::FutureTimeout>()) {
      XLOGF(
          ERR,
          "getGoldenPrefixesPolicyStatus timed out — Rib evb unresponsive");
    } else {
      XLOGF(
          ERR,
          "getGoldenPrefixesPolicyStatus failed: {}",
          ribResult.exception().what());
    }
  }

  goldenPolicy->isPolicyActive() = peerMgr_.getIsGoldenPrefixPolicyActive();
  co_return std::move(goldenPolicy);
}

folly::coro::Task<void> BgpServiceDC::co_removeSafeModeFile() {
  auto log = LOG_THRIFT_CALL(DBG2);

  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  peerMgr_.removeSafeModeFile();
  co_return;
}

folly::coro::Task<std::unique_ptr<rib_policy::TRibPolicy>>
BgpServiceDC::co_getRibPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<rib_policy::TRibPolicy>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<rib_policy::TRibPolicy>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getRibPolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<rib_policy::TRibPolicy>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getRibPolicy timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getRibPolicy failed: {}", result.exception().what());
  }
  co_return std::make_unique<rib_policy::TRibPolicy>();
}

folly::coro::Task<void> BgpServiceDC::co_clearRibPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { rib_.clearRibPolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasException()) {
    if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
      XLOGF(ERR, "clearRibPolicy timed out — Rib evb unresponsive");
    } else {
      XLOGF(ERR, "clearRibPolicy failed: {}", result.exception().what());
    }
  }
}

/**
 * [Route Attribute Policy]
 */
folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setRouteAttributePolicy(
    std::unique_ptr<rib_policy::TRouteAttributePolicy> policy) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || policy == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty policy";
    XLOGF(
        ERR,
        "[{}]: Failed to set route attribute policy: {}",
        kExitNullPtrLogPrefix,
        errStr);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = errStr;
    co_return ret;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this, p = std::move(policy)]() mutable {
        return rib_.setRouteAttributePolicy(std::move(p));
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TResult>(std::move(result.value()));
  }

  auto ret = std::make_unique<TResult>();
  ret->success() = false;
  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "setRouteAttributePolicy timed out — Rib evb unresponsive");
    ret->err() = "Rib evb unresponsive (timeout)";
  } else {
    XLOGF(ERR, "setRouteAttributePolicy failed: {}", result.exception().what());
    ret->err() = std::string(result.exception().what());
  }
  co_return ret;
}

folly::coro::Task<std::unique_ptr<rib_policy::TRouteAttributePolicy>>
BgpServiceDC::co_getRouteAttributePolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<rib_policy::TRouteAttributePolicy>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<rib_policy::TRouteAttributePolicy>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getRouteAttributePolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<rib_policy::TRouteAttributePolicy>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getRouteAttributePolicy timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getRouteAttributePolicy failed: {}", result.exception().what());
  }
  co_return std::make_unique<rib_policy::TRouteAttributePolicy>();
}

folly::coro::Task<void> BgpServiceDC::co_clearRouteAttributePolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { rib_.clearRouteAttributePolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasException()) {
    if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
      XLOGF(ERR, "clearRouteAttributePolicy timed out — Rib evb unresponsive");
    } else {
      XLOGF(
          ERR,
          "clearRouteAttributePolicy failed: {}",
          result.exception().what());
    }
  }
}

/**
 * [Path Selection Policy]
 */
folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setPathSelectionPolicy(
    std::unique_ptr<rib_policy::TPathSelectionPolicy> policy) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || policy == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty policy";
    XLOGF(
        ERR,
        "[{}]: Failed to set path selection policy: {}",
        kExitNullPtrLogPrefix,
        errStr);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = errStr;
    co_return ret;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this, p = std::move(policy)]() mutable {
        return dcRib_.setPathSelectionPolicy(std::move(p));
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TResult>(std::move(result.value()));
  }

  auto ret = std::make_unique<TResult>();
  ret->success() = false;
  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "setPathSelectionPolicy timed out — Rib evb unresponsive");
    ret->err() = "Rib evb unresponsive (timeout)";
  } else {
    XLOGF(ERR, "setPathSelectionPolicy failed: {}", result.exception().what());
    ret->err() = std::string(result.exception().what());
  }
  co_return ret;
}

folly::coro::Task<std::unique_ptr<rib_policy::TPathSelectionPolicy>>
BgpServiceDC::co_getPathSelectionPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<rib_policy::TPathSelectionPolicy>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<rib_policy::TPathSelectionPolicy>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return dcRib_.getPathSelectionPolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<rib_policy::TPathSelectionPolicy>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPathSelectionPolicy timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getPathSelectionPolicy failed: {}", result.exception().what());
  }
  co_return std::make_unique<rib_policy::TPathSelectionPolicy>();
}

folly::coro::Task<void> BgpServiceDC::co_clearPathSelectionPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { dcRib_.clearPathSelectionPolicy(); },
      kRibThriftHandlerTimeout);

  if (result.hasException()) {
    if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
      XLOGF(ERR, "clearPathSelectionPolicy timed out — Rib evb unresponsive");
    } else {
      XLOGF(
          ERR,
          "clearPathSelectionPolicy failed: {}",
          result.exception().what());
    }
  }
}

folly::coro::Task<std::unique_ptr<std::vector<rib_policy::TPathSelector>>>
BgpServiceDC::co_getActivePathSelectionCriteria(
    std::unique_ptr<std::vector<std::string>> prefixes) {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_ || prefixes == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefixes list";
    XLOGF(
        ERR,
        "[{}]: Failed to get path selection criteria. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    co_return std::make_unique<std::vector<rib_policy::TPathSelector>>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<std::vector<rib_policy::TPathSelector>>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this, p = std::move(prefixes)]() mutable {
        return dcRib_.getActivePathSelectionCriteria(std::move(p));
      },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<std::vector<rib_policy::TPathSelector>>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(
        ERR, "getActivePathSelectionCriteria timed out — Rib evb unresponsive");
  } else {
    XLOGF(
        ERR,
        "getActivePathSelectionCriteria failed: {}",
        result.exception().what());
  }
  co_return std::make_unique<std::vector<rib_policy::TPathSelector>>();
}

folly::coro::Task<std::unique_ptr<std::map<std::string, int>>>
BgpServiceDC::co_getGoldenPrefixSubnetCounts() {
  auto log = LOG_THRIFT_CALL(DBG2);
  auto subnetCounts = std::make_unique<std::map<std::string, int>>();
  if (auto& rfPolicy = peerMgr_.getRouteFilterPolicy(); rfPolicy) {
    if (auto goldenPolicy = rfPolicy->getGoldenPrefixPolicy(); goldenPolicy) {
      *subnetCounts = goldenPolicy->getSubnetCounts();
    }
  }
  co_return std::move(subnetCounts);
}

folly::coro::Task<void> BgpServiceDC::co_clearGoldenPrefixesPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }

  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  peerMgr_.clearGoldenPrefixesPolicy();
}

folly::coro::Task<void>
BgpServiceDC::co_clearIngressEgressRouteFiltersPolicy() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return;
  }

  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  peerMgr_.clearIngressEgressRouteFiltersPolicy();
}

folly::coro::Task<std::unique_ptr<TPartialDrainStatus>>
BgpServiceDC::co_getPartialDrainStatus() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<TPartialDrainStatus>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TPartialDrainStatus>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getPartialDrainStatus(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TPartialDrainStatus>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPartialDrainStatus timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getPartialDrainStatus failed: {}", result.exception().what());
  }
  co_return std::make_unique<TPartialDrainStatus>();
}

folly::coro::Task<std::unique_ptr<TPartialDrainState>>
BgpServiceDC::co_getPartialDrainState() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<TPartialDrainState>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<TPartialDrainState>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getPartialDrainState(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<TPartialDrainState>(std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPartialDrainState timed out — Rib evb unresponsive");
  } else {
    XLOGF(ERR, "getPartialDrainState failed: {}", result.exception().what());
  }
  co_return std::make_unique<TPartialDrainState>();
}

folly::coro::Task<std::unique_ptr<std::vector<TPartiallyDrainedPrefix>>>
BgpServiceDC::co_getPartiallyDrainedPrefixes() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>();
  }

  if (!continueExecution(true)) {
    co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>();
  }
  SCOPE_EXIT {
    decrRequestsInExecution();
  };

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this]() { return rib_.getPartiallyDrainedPrefixes(); },
      kRibThriftHandlerTimeout);

  if (result.hasValue()) {
    co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>(
        std::move(result.value()));
  }

  if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
    XLOGF(ERR, "getPartiallyDrainedPrefixes timed out — Rib evb unresponsive");
  } else {
    XLOGF(
        ERR,
        "getPartiallyDrainedPrefixes failed: {}",
        result.exception().what());
  }
  co_return std::make_unique<std::vector<TPartiallyDrainedPrefix>>();
}

void BgpServiceDC::addNetwork(
    std::unique_ptr<TIpPrefix> prefix,
    std::unique_ptr<std::vector<TBgpCommunity>> communities) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || prefix == nullptr || communities == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits"
        : prefix == nullptr      ? "Empty prefix"
                                 : "Empty communities";
    XLOGF(
        ERR,
        "[{}]: Failed to add network. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  std::map<TIpPrefix, TBgpAttributes> networks;
  TBgpAttributes attributes;
  attributes.communities() = *communities;
  networks[*prefix] = std::move(attributes);

  rib_.injectLocalRoutes(networks);
  decrRequestsInExecution();
}

void BgpServiceDC::addNetworks(
    std::unique_ptr<std::map<TIpPrefix, TBgpAttributes>> networks) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || networks == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty network map";
    XLOGF(
        ERR,
        "[{}]: Failed to add networks. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  XLOGF(INFO, "Adding {} networks", networks->size());

  // To avoid CPU hogging on one end and avoid excessive context switches on
  // other extreme, split number of routes to be injected and add in chunks.
  std::map<TIpPrefix, TBgpAttributes> chunkOfNetworks;

  for (auto iter = networks->cbegin(); iter != networks->cend(); ++iter) {
    chunkOfNetworks[iter->first] = iter->second;
    if (chunkOfNetworks.size() == kInjectRouteChunkSize) {
      rib_.injectLocalRoutes(chunkOfNetworks);
      chunkOfNetworks.clear();
    }
    if (FLAGS_thrift_route_add_delay_ms != 0) {
      // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
      std::this_thread::sleep_for(
          milliseconds(FLAGS_thrift_route_add_delay_ms));
    }
  }

  if (chunkOfNetworks.size()) {
    rib_.injectLocalRoutes(chunkOfNetworks);
  }

  decrRequestsInExecution();
}

void BgpServiceDC::delNetwork(std::unique_ptr<TIpPrefix> prefix) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || prefix == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefix";
    XLOGF(
        ERR,
        "[{}]: Failed to delete network. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  std::set<TIpPrefix> chunkOfPrefixes{*prefix};

  rib_.removeLocalRoutes(chunkOfPrefixes);
  decrRequestsInExecution();
}

void BgpServiceDC::delNetworks(std::unique_ptr<std::set<TIpPrefix>> prefixes) {
  auto log = LOG_THRIFT_CALL(INFO);
  if (exitInitiated_ || prefixes == nullptr) {
    auto errStr = exitInitiated_ ? "Session exits" : "Empty prefixes list";
    XLOGF(
        ERR,
        "[{}]: Failed to delete networks. Error: {}",
        kExitNullPtrLogPrefix,
        errStr);
    return;
  }

  if (!continueExecution(true)) {
    return;
  }

  XLOGF(INFO, "Deleting {} prefixes", prefixes->size());

  // To avoid CPU hogging on one end and avoid excessive context switches on
  // other extreme, split number of prefixes to be withdrawn and add in chunks.
  std::set<TIpPrefix> chunkOfPrefixes;

  for (auto iter = prefixes->cbegin(); iter != prefixes->cend(); ++iter) {
    chunkOfPrefixes.insert(*iter);
    if (chunkOfPrefixes.size() == kInjectRouteChunkSize) {
      rib_.removeLocalRoutes(chunkOfPrefixes);
      chunkOfPrefixes.clear();
    }
    // Let the service thread sleep without overloading Rib thread
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (chunkOfPrefixes.size()) {
    rib_.removeLocalRoutes(chunkOfPrefixes);
  }

  decrRequestsInExecution();
}

/**
 * [Route Filter Policy — FILE_MODE gating]
 */
folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setRouteFilterPolicy(
    std::unique_ptr<rib_policy::TRouteFilterPolicy> policy) {
  auto crfLock = co_await crfPolicyMutex_.co_scoped_lock_shared();

  if (ribDC_.isCrfFileModeEnabled()) {
    XLOGF(WARN, "[CRF] CRF policy is in FILE_MODE, cannot set via Thrift RPC");
    BgpStats::incrCrfThriftRpcRejected();
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "CRF policy is in FILE_MODE, cannot set via Thrift RPC";
    co_return ret;
  }

  // @lint-ignore CLANGTIDY facebook-thrift-handler-direct-call
  co_return co_await BgpServiceBase::co_setRouteFilterPolicy(std::move(policy));
}

folly::coro::Task<void> BgpServiceDC::co_clearRouteFilterPolicy() {
  auto crfLock = co_await crfPolicyMutex_.co_scoped_lock_shared();

  if (ribDC_.isCrfFileModeEnabled()) {
    XLOGF(WARN, "[CRF] Cannot clear CRF policy while in FILE_MODE");
    BgpStats::incrCrfThriftRpcRejected();
    co_return;
  }

  // @lint-ignore CLANGTIDY facebook-thrift-handler-direct-call
  co_await BgpServiceBase::co_clearRouteFilterPolicy();
}

folly::coro::Task<std::unique_ptr<TResult>>
BgpServiceDC::co_setCrfPolicyFromFile() {
  auto log = LOG_THRIFT_CALL(DBG2);
  if (exitInitiated_) {
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Session exits";
    co_return ret;
  }

  // Read artifact outside the lock to avoid blocking Thrift CRF RPCs on I/O
  auto artifact = readThriftArtifactFromFile<rib_policy::CrfPolicyArtifact>(
      FLAGS_crf_policy_file);
  if (artifact.hasError()) {
    /*
     * Only kError (file present but unreadable/corrupt) is a read failure.
     * kAbsent (no path configured or file not present) still fails the RPC
     * below, but is not counted so bgpd.crf.artifact_read.failure keeps one
     * consistent meaning fleet-wide (genuine read/parse errors only), matching
     * the startup bootstrap path in RibDC.
     */
    if (artifact.error() == ArtifactReadError::kError) {
      BgpStats::incrCrfArtifactReadFailure();
    }
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Failed to read CRF policy artifact file";
    co_return ret;
  }
  BgpStats::incrCrfArtifactReadSuccess();

  auto crfLock = co_await crfPolicyMutex_.co_scoped_lock();

  if (exitInitiated_) {
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = "Session exits";
    co_return ret;
  }

  bool fileMode = !*artifact->dryrun();

  if (!fileMode) {
    /*
     * Transition to THRIFT_MODE. The previously-applied CRF policy (if any)
     * remains active in PeerManager and RIB — only the gating flag changes.
     * Clearing the policy here would cause unnecessary traffic disruption.
     */
    ribDC_.setCrfFileModeEnabled(false);
    auto ret = std::make_unique<TResult>();
    ret->success() = true;
    ret->err() =
        "CRF artifact dryrun=true, staying in THRIFT_MODE (no policy applied)";
    co_return ret;
  }

  auto policy = std::make_unique<rib_policy::TRouteFilterPolicy>(
      std::move(*artifact->policy()));

  neteng::fboss::bgp::thrift::TResult validationResult;
  auto config = configManager_->getConfig();
  validatePeerGroupConfigInPolicy(
      validationResult, *policy, config->getPeerGroups());
  if (!*validationResult.success()) {
    XLOGF(
        ERR,
        "[CRF] Failed to validate file-based CRF policy. Error: {}",
        *validationResult.err());
    BgpStats::incrCrfPolicyAppliedFailure();
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = *validationResult.err();
    co_return ret;
  }

  bool wasFileModeActive = ribDC_.isCrfFileModeEnabled();
  ribDC_.setCrfFileModeEnabled(true);
  continueExecution(false);
  SCOPE_EXIT {
    decrRequestsInExecution();
  };
  try {
    peerMgr_.setRouteFilterPolicy(
        std::make_unique<RouteFilterPolicy>(*policy), /*forceUpdate=*/true);
  } catch (const BgpError& ex) {
    if (!wasFileModeActive) {
      ribDC_.setCrfFileModeEnabled(false);
    }
    BgpStats::incrCrfPolicyAppliedFailure();
    auto errorMsg = folly::exceptionStr(ex);
    XLOGF(ERR, "[CRF] {}", errorMsg);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = std::string(errorMsg);
    co_return ret;
  } catch (const std::exception& ex) {
    if (!wasFileModeActive) {
      ribDC_.setCrfFileModeEnabled(false);
    }
    BgpStats::incrCrfPolicyAppliedFailure();
    auto errorMsg = folly::exceptionStr(ex);
    XLOGF(ERR, "[CRF] Unexpected error applying CRF policy: {}", errorMsg);
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    ret->err() = std::string(errorMsg);
    co_return ret;
  }

  auto result = co_await co_runOnEvbWithTimeout(
      rib_.getEventBase(),
      [this, p = std::move(policy)]() mutable {
        rib_.setRouteFilterPolicy(std::move(p), /*forceUpdate=*/true);
      },
      kRibThriftHandlerTimeout);

  if (result.hasException()) {
    BgpStats::incrCrfPolicyAppliedFailure();
    auto ret = std::make_unique<TResult>();
    ret->success() = false;
    if (result.exception().is_compatible_with<folly::FutureTimeout>()) {
      XLOGF(ERR, "[CRF] setCrfPolicyFromFile timed out — Rib evb unresponsive");
      ret->err() = "Rib evb unresponsive (timeout)";
    } else {
      XLOGF(
          ERR,
          "[CRF] setCrfPolicyFromFile failed: {}",
          result.exception().what());
      ret->err() = std::string(result.exception().what());
    }
    co_return ret;
  }

  BgpStats::incrCrfPolicyAppliedSuccess();
  auto ret = std::make_unique<TResult>();
  ret->success() = true;
  ret->err() = "CRF policy applied from file (FILE_MODE)";
  co_return ret;
}

} // namespace facebook::bgp
