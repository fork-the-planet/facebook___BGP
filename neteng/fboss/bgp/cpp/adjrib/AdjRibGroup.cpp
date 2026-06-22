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

#include <folly/Poly.h>
#include <folly/Portability.h>
#include <folly/ScopeGuard.h>
#include <folly/Singleton.h>
#include <folly/coro/BlockingWait.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Sleep.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

#include "fboss/agent/AddressUtil.h"
#include "neteng/fboss/bgp/cpp/BgpProfiler.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibCommon.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibGroup.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibGroupSerializer.h"
#include "neteng/fboss/bgp/cpp/adjrib/WellKnownCommunityFilter.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ConsumerBitmap.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

namespace facebook::bgp {

// Global cache for policy result strings (shared with AdjRib)
// This is needed because AdjRibEntry::setPostPolicy() references it
extern PostPolicyResultCacheT postPolicyResultCache_;

AdjRibOutGroup::~AdjRibOutGroup() {
  XLOGF(INFO, "Destroyed update group: {}", groupDescriptor_);
}

/*
 * @brief Cooperatively drain asyncScope_ before group destruction.
 *
 * Cleanup sequence:
 * 1. Request cancellation of all pending coroutines
 * 2. Wait for all coroutines to complete (joinAsync)
 *
 * - asyncScope_ holds coroutines for deferredPushToPeer operations
 * - Without cleanup, coroutines may still be running when group is destroyed
 * - This causes use-after-free when coroutines access group members
 *
 * Must be co_awaited before the group is destroyed. Called by
 * UpdateGroupManager::maybeDestroyUpdateGroup().
 */
folly::coro::Task<void> AdjRibOutGroup::drainAsyncScope() {
  XLOGF(
      DBG1,
      "Group {} drainAsyncScope: canceling pending async operations",
      groupDescriptor_);

  asyncScope_.requestCancellation();
  co_await asyncScope_.joinAsync();

  XLOGF(
      DBG1,
      "Group {} drainAsyncScope: async cleanup complete",
      groupDescriptor_);
}

void AdjRibOutGroup::deleteFromPathTree(
    AdjRibPathTree& pathTree,
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& ownerKey,
    uint32_t pathId) noexcept {
  deleteFromPathTree(
      pathTree,
      getRadixNodeItrFromPathTree(pathTree, prefix),
      ownerKey,
      pathId);
}

void AdjRibOutGroup::deleteFromPathTree(
    AdjRibPathTree& pathTree,
    AdjRibPathTree::Iterator&& itr,
    const AdjRibOutOwnerKey& ownerKey,
    uint32_t pathId) noexcept {
  if (itr.atEnd()) {
    /*
     * Prefix does not exist, nothing to delete
     */
    return;
  }

  auto ownerItr = itr.value().find(ownerKey);
  if (ownerItr == itr.value().end()) {
    /*
     * owner for prefix does not exist, nothing to delete
     */
    return;
  }

  auto pathItr = ownerItr->second.find(pathId);
  if (pathItr == ownerItr->second.end()) {
    /*
     * path entry does not exist, nothing to delete
     */
    return;
  }

  ownerItr->second.erase(pathId);
  if (ownerItr->second.size() == 0) {
    /*
     * No paths for this owner in a given prefix, delete owner
     */
    itr.value().erase(ownerItr);
  }

  if (itr.value().size() == 0) {
    /*
     * No owner for this prefix, delete prefix
     */
    pathTree.erase(itr);
  }
}

void AdjRibOutGroup::deleteFromLiteTree(
    AdjRibLiteTree& liteTree,
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& ownerKey) noexcept {
  deleteFromLiteTree(
      liteTree, getRadixNodeItrFromLiteTree(liteTree, prefix), ownerKey);
}

void AdjRibOutGroup::deleteFromLiteTree(
    AdjRibLiteTree& liteTree,
    AdjRibLiteTree::Iterator&& itr,
    const AdjRibOutOwnerKey& ownerKey) noexcept {
  if (itr.atEnd()) {
    /*
     * Prefix does not exist, nothing to delete
     */
    return;
  }

  auto ownerItr = itr.value().find(ownerKey);
  if (ownerItr == itr.value().end()) {
    /*
     * owner for prefix does not exist, nothing to delete
     */
    return;
  }

  itr.value().erase(ownerItr);
  if (itr.value().size() == 0) {
    /*
     * No owner for this prefix, delete prefix
     */
    liteTree.erase(itr);
  }
}

AdjRibEntry* FOLLY_NONNULL AdjRibOutGroup::addToPathTree(
    AdjRibPathTree& pathTree,
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& ownerKey,
    uint32_t pathId) noexcept {
  folly::F14ValueMap<uint32_t, std::unique_ptr<AdjRibEntry>> paths;

  auto radixNodeItr = getRadixNodeItrFromPathTree(pathTree, prefix);
  if (radixNodeItr.atEnd()) {
    folly::F14ValueMap<
        AdjRibOutOwnerKey,
        folly::F14ValueMap<uint32_t, std::unique_ptr<AdjRibEntry>>,
        AdjRibOutOwnerKeyHash>
        owners;

    owners[ownerKey] = std::move(paths);
    radixNodeItr =
        pathTree.insert(prefix.first, prefix.second, std::move(owners)).first;
  } else {
    if (radixNodeItr.value().find(ownerKey) == radixNodeItr.value().end()) {
      radixNodeItr.value()[ownerKey] = std::move(paths);
    }
  }

  auto ownerItr = radixNodeItr.value().find(ownerKey);
  ownerItr->second[pathId] = std::make_unique<AdjRibEntry>(pathId);
  return ownerItr->second.at(pathId).get();
}

AdjRibEntry* FOLLY_NONNULL AdjRibOutGroup::addToLiteTree(
    AdjRibLiteTree& liteTree,
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& ownerKey,
    uint32_t pathId) noexcept {
  auto radixNodeItr = getRadixNodeItrFromLiteTree(liteTree, prefix);
  if (radixNodeItr.atEnd()) {
    folly::F14ValueMap<
        AdjRibOutOwnerKey,
        std::unique_ptr<AdjRibEntry>,
        AdjRibOutOwnerKeyHash>
        owners;
    radixNodeItr =
        liteTree.insert(prefix.first, prefix.second, std::move(owners)).first;
  }

  radixNodeItr.value()[ownerKey] = std::make_unique<AdjRibEntry>(pathId);
  return radixNodeItr.value().at(ownerKey).get();
}

AdjRibEntry* FOLLY_NULLABLE AdjRibOutGroup::getFromPathTree(
    AdjRibPathTree& pathTree,
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& ownerKey,
    uint32_t pathId) noexcept {
  auto radixNodeItr = getRadixNodeItrFromPathTree(pathTree, prefix);
  return getAdjRibEntryFromPathNodeItr(radixNodeItr, ownerKey, pathId);
}

AdjRibEntry* FOLLY_NULLABLE AdjRibOutGroup::getAdjRibEntryFromPathNodeItr(
    const AdjRibPathTree::Iterator& itr,
    const AdjRibOutOwnerKey& ownerKey,
    uint32_t pathId) noexcept {
  if (itr.atEnd()) {
    /*
     * Prefix does not exist
     */
    return nullptr;
  }

  auto ownerItr = itr.value().find(ownerKey);
  if (ownerItr == itr.value().end()) {
    /*
     * Owner entry for that specific prefix does not exist
     */
    return nullptr;
  }

  auto pathItr = ownerItr->second.find(pathId);
  if (pathItr == ownerItr->second.end()) {
    /*
     * Path entry specific to prefix/owner does not exist
     */
    return nullptr;
  }

  return pathItr->second.get();
}

AdjRibEntry* FOLLY_NULLABLE AdjRibOutGroup::getFromLiteTree(
    AdjRibLiteTree& liteTree,
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& ownerKey) noexcept {
  auto radixNodeItr = getRadixNodeItrFromLiteTree(liteTree, prefix);
  return getAdjRibEntryFromLiteNodeItr(radixNodeItr, ownerKey);
}

AdjRibEntry* FOLLY_NULLABLE AdjRibOutGroup::getAdjRibEntryFromLiteNodeItr(
    const AdjRibLiteTree::Iterator& itr,
    const AdjRibOutOwnerKey& ownerKey) noexcept {
  if (itr.atEnd()) {
    /*
     * Prefix does not exist
     */
    return nullptr;
  }

  auto ownerItr = itr.value().find(ownerKey);
  if (ownerItr == itr.value().end()) {
    /*
     * Owner entry for that specific prefix does not exist
     */
    return nullptr;
  }

  return ownerItr->second.get();
}

AdjRibOutGroup::AdjRibPathTree::Iterator
AdjRibOutGroup::getRadixNodeItrFromPathTree(
    AdjRibPathTree& pathTree,
    const folly::CIDRNetwork& prefix) noexcept {
  return pathTree.exactMatch(prefix.first, prefix.second);
}

AdjRibOutGroup::AdjRibLiteTree::Iterator
AdjRibOutGroup::getRadixNodeItrFromLiteTree(
    AdjRibLiteTree& liteTree,
    const folly::CIDRNetwork& prefix) noexcept {
  return liteTree.exactMatch(prefix.first, prefix.second);
}

AdjRibEntry* AdjRibOutGroup::addRibEntry(
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& ownerKey,
    uint32_t pathId) noexcept {
  if (groupKey_.sendAddPath) {
    return addToPathTree(PathTree_, prefix, ownerKey, pathId);
  }
  return addToLiteTree(LiteTree_, prefix, ownerKey, pathId);
}

std::pair<AdjRibEntry * FOLLY_NULLABLE, bool>
AdjRibOutGroup::getRibPathEntrySharedOrPeer(
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& peerOwnerKey,
    uint32_t pathId) noexcept {
  auto itr = getRadixNodeItrFromPathTree(PathTree_, prefix);
  auto* entry = getAdjRibEntryFromPathNodeItr(itr, peerOwnerKey, pathId);
  if (entry) {
    return {entry, true /* isPerPeerEntry */};
  }
  return {
      getAdjRibEntryFromPathNodeItr(itr, getGroupOwnerKey(), pathId),
      false /* isPerPeerEntry */};
}

std::pair<AdjRibEntry * FOLLY_NULLABLE, bool>
AdjRibOutGroup::getRibLiteEntrySharedOrPeer(
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& peerOwnerKey) noexcept {
  auto itr = getRadixNodeItrFromLiteTree(LiteTree_, prefix);
  auto* entry = getAdjRibEntryFromLiteNodeItr(itr, peerOwnerKey);
  if (entry) {
    return {entry, true /* isPerPeerEntry */};
  }
  return {
      getAdjRibEntryFromLiteNodeItr(itr, getGroupOwnerKey()),
      false /* isPerPeerEntry */};
}

std::pair<AdjRibEntry * FOLLY_NULLABLE, bool>
AdjRibOutGroup::getRibEntrySharedOrPeer(
    const folly::CIDRNetwork& prefix,
    const AdjRibOutOwnerKey& peerOwnerKey,
    uint32_t pathId,
    uint64_t detachedRibVersion) noexcept {
  auto [entry, isPerPeerEntry] = groupKey_.sendAddPath
      ? getRibPathEntrySharedOrPeer(prefix, peerOwnerKey, pathId)
      : getRibLiteEntrySharedOrPeer(prefix, peerOwnerKey);
  if (isPerPeerEntry) {
    return {entry, true /* isPerPeerEntry */};
  }
  if (entry && isEntryNotShared(detachedRibVersion, entry->getRibVersion())) {
    return {nullptr, false /* isPerPeerEntry */};
  }
  return {entry, false /* isPerPeerEntry */};
}

uint32_t AdjRibOutGroup::getPeerEntriesCountFromPathTree(
    AdjRibPathTree& pathTree,
    const AdjRibOutOwnerKey& ownerKey) noexcept {
  uint32_t size = 0;

  for (auto itr = pathTree.begin(); itr != pathTree.end(); itr++) {
    if (itr->value().find(ownerKey) != itr->value().end()) {
      size += itr->value().at(ownerKey).size();
    }
  }

  return size;
}

uint32_t AdjRibOutGroup::getPeerEntriesCountFromLiteTree(
    AdjRibLiteTree& liteTree,
    const AdjRibOutOwnerKey& ownerKey) noexcept {
  uint32_t size = 0;

  for (auto itr = liteTree.begin(); itr != liteTree.end(); itr++) {
    if (itr->value().find(ownerKey) != itr->value().end()) {
      size++;
    }
  }

  return size;
}

/*
 * @brief  Activate change list consumer for this update group
 *         Registers group with change tracker and starts polled consumption
 *         Same pattern as AdjRib::activateChangeListConsumer
 *
 * @param  none
 *
 * @return void
 */
void AdjRibOutGroup::activateChangeListConsumer() noexcept {
  if (!changeListConsumer_) {
    XLOGF(
        ERR,
        "changeListConsumer is not initialized and hence cannot be registered for group {}",
        groupDescriptor_);
    return;
  }

  if (changeListConsumeTimer_) {
    XLOGF(
        ERR,
        "update group {} has already been registered to changeListTracker",
        groupDescriptor_);
    return;
  }

  XLOGF(
      DBG1, "Register update group {} to changeListTracker", groupDescriptor_);

  // Register with tracker in polled mode
  changeListConsumer_->registerWithTracker();
  changeListConsumer_->setPolledMode();
  changeListConsumer_->setBitmap();

  changeListConsumeTimer_ = folly::AsyncTimeout::make(evb_, [this]() noexcept {
    /*
     * Start a polled cycle to consume available changes.
     * We use asyncScope_.add(co_withExecutor(...)) to track the coroutine
     * rather than running the work inline in the timer callback. This enqueues
     * the coroutine as a new EventBase callback, allowing already-pending
     * callbacks (peer session events, socket reads, other timers) to drain
     * first — important under scale when many peers flap simultaneously.
     * Tracking via asyncScope_ ensures the coroutine is cancelled during
     * group destruction, preventing use-after-free.
     */
    asyncScope_.add(co_withExecutor(
        &evb_, folly::coro::co_invoke([this]() -> folly::coro::Task<void> {
          /*
           * Guard against use-after-reset: the MRAI timer fires and
           * enqueues this coroutine, but between scheduling and execution,
           * other EventBase callbacks may run — including session
           * termination paths that call deactivateChangeListConsumer()
           * and nullify changeListConsumer_. Under scale with mass peer
           * flaps, this window is wide enough to hit consistently.
           */
          if (!changeListConsumer_) {
            XLOGF(
                WARN,
                "Group {}: CL consume timer fired but "
                "changeListConsumer_ is null, skipping",
                groupDescriptor_);
            co_return;
          }
          // Use iterator-based interface for consuming change items
          auto previousRibVersion = lastSeenRibVersion_;
          {
            ScopedProfile profile("AdjRibOutGroup::consumeChangeList");
            changeListConsumer_->iterateChanges();
          }
          if (changeListConsumer_->isStale(kConsumerStalenessThreshold) &&
              !changeListConsumer_->isStalenessLogged()) {
            XLOGF(
                WARN,
                "Group {}: change list consumer stale for {}ms, marker has not advanced",
                groupDescriptor_,
                changeListConsumer_->stalenessDuration().count());
            changeListConsumer_->markStalenessLogged();
          }
          XLOGF(
              DBG2,
              "Group {}: Updating cached RIB version from {} to {}",
              groupDescriptor_,
              previousRibVersion,
              lastSeenRibVersion_);

          if (changeListConsumeTimer_) {
            changeListConsumeTimer_->scheduleTimeout(
                std::chrono::milliseconds(mraiInterval_));
          }
          /* Trigger message building if packing list has entries */
          if (!attrToPrefixMap_.empty() &&
              (state_ == UpdateGroupState::READY ||
               state_ == UpdateGroupState::IDLE)) {
            state_ = UpdateGroupState::WAITING;
            asyncScope_.add(
                folly::coro::co_withExecutor(
                    &evb_, buildAndSendGroupBgpMessages(false)));
          }
          co_return;
        })));
  });
}

void AdjRibOutGroup::scheduleConsumeTimer() noexcept {
  if (!changeListConsumeTimer_) {
    XLOGF(
        ERR,
        "Group {}: cannot schedule consume timer — timer not created",
        groupDescriptor_);
    return;
  }
  if (changeListConsumeTimer_->isScheduled()) {
    return;
  }
  changeListConsumeTimer_->scheduleTimeout(
      std::chrono::milliseconds(mraiInterval_));
}

/*
 * @brief  Deactivate change list consumer for this update group
 *         Unregisters group from change tracker and cancels timer
 *         Same pattern as AdjRib::deactivateChangeListConsumer
 *
 * @param  none
 *
 * @return void
 */
void AdjRibOutGroup::deactivateChangeListConsumer() noexcept {
  // cleanup any pending attrToPrefixMap_
  clearPackingList();

  if (changeListConsumeTimer_) {
    changeListConsumeTimer_->cancelTimeout();
    changeListConsumeTimer_.reset();
  }

  /*
   * Always deregister the consumer, even if timer wasn't active.
   * This ensures bitPosition_ is set to -1 so the consumer destructor
   * won't try to access the (possibly destroyed) tracker.
   */
  if (changeListConsumer_) {
    changeListConsumer_->terminate();
    changeListConsumer_->resetBitmap();
    changeListConsumer_->deregisterFromTracker();
    XLOGF(
        DBG1, "Deregistered group {} from changeListTracker", groupDescriptor_);
  }
}

/*
 * @brief  Schedule initial dump to start asynchronously
 *         Transitions state from UNINITIALIZED to building packing list
 *
 * @param  none
 *
 * @return void
 */
void AdjRibOutGroup::scheduleInitialDump() noexcept {
  if (state_ != UpdateGroupState::UNINITIALIZED) {
    XLOGF(
        WARN,
        "Group {} not in UNINITIALIZED state, skipping initial dump",
        groupDescriptor_);
    return;
  }

  XLOGF(INFO, "Scheduling initial RIB dump for group {}", groupDescriptor_);

  // Start the coroutine to build initial dump
  // Only proceed if update group feature is enabled
  if (enableUpdateGroup_) {
    asyncScope_.add(
        co_withExecutor(&evb_, buildAndScheduleSendInitialDumpFromShadowRib()));
  }
}

/*
 * Shared core: walk ShadowRib and process all entries through
 * processRibOutAnnouncement().
 * Used by both initial dump and policy re-evaluation.
 */
uint64_t AdjRibOutGroup::walkAndProcessShadowRib(bool sendWithEoR) {
  // Build announcement struct, same as PeerManager::processRibDumpReq
  RibOutAnnouncement announcement;
  announcement.initialDump = true;

  // Track max RIB version seen during walk
  uint64_t maxRibVersion = 0;

  // Walk through all shadow RIB entries
  // NOTE: this ensures maximum packing without chunk limit.
  for (const auto& [prefix, srEntryPtr] : *shadowRibEntries_) {
    // TODO: add cancellable token to make sure this iteration is interruptible
    if (!srEntryPtr) {
      continue;
    }

    const auto& srEntry = srEntryPtr->get();

    // Track max version across all entries
    if (srEntry.ribVersion > maxRibVersion) {
      maxRibVersion = srEntry.ribVersion;
    }

    if (!groupKey_.sendAddPath) {
      // Send out bestpath only (no add-path)
      const auto bestpath = srEntry.bestpath;
      if (bestpath) {
        if (isShadowRibRouteInWithdraw(bestpath->flags)) {
          continue;
        }
        announcement.entries.emplace_back(
            prefix,
            kDefaultPathID,
            bestpath->peer,
            bestpath->attrs,
            srEntry.switchId,
            srEntry.multiPathSize,
            srEntry.aggregateReceivedUcmpWeight,
            srEntry.aggregateLocalUcmpWeight,
            srEntry.ribPolicyUcmpWeight,
            srEntry.newlyInstalledInLocalRib,
            srEntry.installTimeStamp,
            srEntry.ribVersion,
            bestpath->isPartialDrain);
      }
    } else {
      // Send out all multipaths with add-path enabled
      for (const auto& [_, multipath] : srEntry.multipaths) {
        if (multipath) {
          if (isShadowRibRouteInWithdraw(multipath->flags)) {
            continue;
          }
          announcement.addPathEntries.emplace_back(
              prefix,
              multipath->pathIdToSend,
              multipath->peer,
              multipath->attrs,
              srEntry.switchId,
              srEntry.multiPathSize,
              srEntry.aggregateReceivedUcmpWeight,
              srEntry.aggregateLocalUcmpWeight,
              srEntry.ribPolicyUcmpWeight,
              srEntry.newlyInstalledInLocalRib,
              srEntry.installTimeStamp,
              srEntry.ribVersion,
              multipath->isPartialDrain);
        }
      }
    }
  }

  announcement.sendWithEoR = sendWithEoR;

  XLOGF(
      INFO,
      "Group {} walkAndProcessShadowRib completed with {} entries "
      "(sendAddPath={}, sendWithEoR={})",
      groupDescriptor_,
      groupKey_.sendAddPath ? announcement.addPathEntries.size()
                            : announcement.entries.size(),
      groupKey_.sendAddPath,
      sendWithEoR);

  /*
   * Process the announcement - this schedules async build and send
   * via buildAndSendGroupBgpMessages()
   */
  processRibOutAnnouncement(announcement);

  return maxRibVersion;
}

/*
 * Build initial RIB dump from shadow RIB.
 * Walks shadow RIB, builds RibOutAnnouncement, processes it.
 * Transitions from UNINITIALIZED to WAITING state.
 */
uint64_t AdjRibOutGroup::processRibDumpForGroup() {
  XLOGF(
      INFO, "Group {} starting initial dump from shadow RIB", groupDescriptor_);

  if (!shadowRibEntries_) {
    XLOGF(
        WARN,
        "No shadow RIB reference for group {}, completing dump with empty list",
        groupDescriptor_);
    state_ = UpdateGroupState::IDLE;
    return lastSeenRibVersion_;
  }

  auto maxRibVersion = walkAndProcessShadowRib(true /* sendWithEoR */);

  /*
   * Transition to WAITING state - ready to send updates
   * Note: Initial dump sent with EoR marker
   */
  state_ = UpdateGroupState::WAITING;

  /*
   * Set cached RIB version on the group after initial dump.
   * All members of an update group share this version since they
   * receive updates as a unit. Display logic reads from group.
   */
  XLOGF(
      DBG2,
      "Group {}: Updating cached RIB version from {} to {} after rib walk",
      groupDescriptor_,
      lastSeenRibVersion_,
      maxRibVersion);
  setLastSeenRibVersion(maxRibVersion);

  initialDumpCompletionTimeMs_ =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count();

  return lastSeenRibVersion_;
}

folly::coro::Task<void>
AdjRibOutGroup::buildAndScheduleSendInitialDumpFromShadowRib() {
  auto lastSeenRibVersion = processRibDumpForGroup();

  if (state_ == UpdateGroupState::IDLE) {
    co_return;
  }

  /*
   * Transition INIT peers to JOINED_RUNNING
   * These peers were waiting for initial dump and are now active
   * Note: sync bitmap bit already set when they registered
   */
  for (const auto& [bitPos, adjRib] : bitToAdjRibs_) {
    if (adjRib && adjRib->getPeerState() == PeerUpdateState::INIT) {
      XLOGF(
          DBG1,
          "Group {}: Peer {} at bit {} State Transition: {} -> {}",
          groupDescriptor_,
          adjRib->getPeerName(),
          bitPos,
          adjRib->getPeerState(),
          PeerUpdateState::JOINED_RUNNING);
      adjRib->setPeerState(PeerUpdateState::JOINED_RUNNING);
      adjRib->setLastSeenRibVersion(lastSeenRibVersion);
    }
  }

  activateChangeListConsumer();
  scheduleConsumeTimer();
}

/*
 * Re-evaluate all ShadowRib entries with the current egress policy.
 * Called when the group's egress policy content changes.
 *
 * Skips if group is UNINITIALIZED (initial dump will use latest policy).
 * Reuses walkAndProcessShadowRib() for the walk. During processing,
 * processRibAnnouncedEntryForGroup() does deep compare with existing
 * group AdjRibOut entries and only updates the packing list when the
 * policy result actually changed. Lazy clone (Phase 4) preserves
 * detached peers' old-policy state before group entries are mutated.
 */
void AdjRibOutGroup::reEvaluateSyncPeersEgressPolicy() {
  if (state_ == UpdateGroupState::UNINITIALIZED) {
    XLOGF(
        INFO,
        "Group {}: Skipping egress policy re-evaluation, group is UNINITIALIZED",
        groupDescriptor_);
    return;
  }

  if (!shadowRibEntries_) {
    XLOGF(
        WARN,
        "Group {}: No shadow RIB reference for egress policy re-evaluation",
        groupDescriptor_);
    return;
  }

  XLOGF(
      INFO,
      "Group {}: Starting egress policy re-evaluation for sync peers",
      groupDescriptor_);

  walkAndProcessShadowRib(false /* sendWithEoR */);

  XLOGF(
      INFO,
      "Group {}: Finished egress policy re-evaluation for sync peers",
      groupDescriptor_);
}

/*
 * @brief  Process ShadowRibEntry change from change tracker
 *         Converts shadow RIB entries to announcements at group level
 *
 * Similar to AdjRib::processShadowRibEntryChange but operates at group level.
 * This is called by AdjRibOutGroupConsumer::processChangeItem() when consuming
 * changes from the tracker.
 *
 * @param  srEntry - shadow Rib entry consumed from the changeList
 *
 * @return void
 */
void AdjRibOutGroup::processShadowRibEntryChange(
    ShadowRibEntry& srEntry) noexcept {
  setLastSeenRibVersion(srEntry.ribVersion);

  /*
   * Convert shadow RIB entry to announcement entries.
   * Same pattern as AdjRib::processShadowRibEntryChange
   */
  RibOutAnnouncement announcement;
  announcement.initialDump = false;

  if (groupKey_.sendAddPath) {
    // Send out all multipaths with add-path enabled
    for (const auto& [_, multipath] : srEntry.multipaths) {
      if (!multipath) {
        continue;
      }
      if (isShadowRibRouteInUpdate(multipath->flags)) {
        RibOutAnnouncementEntry entry(
            srEntry.prefix,
            multipath->pathIdToSend,
            multipath->peer,
            multipath->attrs,
            srEntry.switchId,
            srEntry.multiPathSize,
            srEntry.aggregateReceivedUcmpWeight,
            srEntry.aggregateLocalUcmpWeight,
            srEntry.ribPolicyUcmpWeight);
        entry.newlyInstalledInLocalRib = srEntry.newlyInstalledInLocalRib;
        entry.installTimeStamp = srEntry.installTimeStamp;
        entry.ribVersion = srEntry.ribVersion;
        entry.isPartialDrain = multipath->isPartialDrain;
        announcement.addPathEntries.push_back(entry);
      } else if (isShadowRibRouteInWithdraw(multipath->flags)) {
        /*
         * Withdrawal announcements from RIB don't have attrs, however
         * when we are processing withdrawal here, it is a priori
         * shadow RIB entry that was existing with attrs in place (the
         * entry yet has not been reset for multipath) and hence it
         * should have attrs available to get the nexthop value
         *
         * Thus, we would never expect nullptr to attrs here. The check
         * here is simply a safety check
         */
        if (multipath->attrs) {
          // Process withdrawal for this path
          processGroupRibWithdraw(srEntry.prefix, multipath->pathIdToSend);
        } else {
          XLOGF(
              ERR,
              "Unexpected null attr for multipath when processing "
              "withdrawal for shadowrib entry. prefix {}, group {}",
              folly::IPAddress::networkToString(srEntry.prefix),
              groupDescriptor_);
        }
      }
    }
  } else {
    // Send out bestpath only (no add-path)
    if (!srEntry.bestpath) {
      return;
    }
    if (isShadowRibRouteInUpdate(srEntry.bestpath->flags)) {
      RibOutAnnouncementEntry entry(
          srEntry.prefix,
          kDefaultPathID,
          srEntry.bestpath->peer,
          srEntry.bestpath->attrs,
          srEntry.switchId,
          srEntry.multiPathSize,
          srEntry.aggregateReceivedUcmpWeight,
          srEntry.aggregateLocalUcmpWeight,
          srEntry.ribPolicyUcmpWeight);
      entry.newlyInstalledInLocalRib = srEntry.newlyInstalledInLocalRib;
      entry.installTimeStamp = srEntry.installTimeStamp;
      entry.ribVersion = srEntry.ribVersion;
      entry.isPartialDrain = srEntry.bestpath->isPartialDrain;
      announcement.entries.push_back(entry);
    } else if (isShadowRibRouteInWithdraw(srEntry.bestpath->flags)) {
      /*
       * Withdrawal announcements from RIB don't have attrs, however
       * when we are processing withdrawal here, it is a priori
       * shadow RIB entry that was existing with attrs in place (the
       * entry yet has not been reset for bestpath) and hence it
       * should have attrs available to get the nexthop value
       *
       * Thus, we would never expect nullptr to attrs here. The check
       * here is simply a safety check
       */
      if (srEntry.bestpath->attrs) {
        // Process withdrawal for bestpath
        processGroupRibWithdraw(srEntry.prefix, kDefaultPathID);
      } else {
        XLOGF(
            ERR,
            "Unexpected null attr for bestpath when processing "
            "withdrawal for shadowrib entry. prefix {}, group {}",
            folly::IPAddress::networkToString(srEntry.prefix),
            groupDescriptor_);
      }
    }
  }

  if (!announcement.entries.empty() || !announcement.addPathEntries.empty()) {
    processRibOutAnnouncement(announcement);
  }
}

/*
 * @brief  Process withdrawal for the group
 *         Similar to AdjRib::processRibWithdraw but at group level
 *
 * Key differences from peer-level:
 * - No deferred updates (out-delay) - not implemented yet for groups (future
 * diff)
 * - No AFI negotiation check - groups inherit AFI from peer membership
 * - No per-prefix stats tracking like AdjRib - groups optimize for memory
 * - Inline withdrawal logic instead of separate tryInsertWithdrawal() method
 * - No RibOutWithdrawalEntry struct - simpler direct call
 *
 * @param  prefix - Prefix to withdraw
 * @param  pathId - Path ID to withdraw
 *
 * @return void
 */
void AdjRibOutGroup::processGroupRibWithdraw(
    const folly::CIDRNetwork& prefix,
    uint32_t pathId) noexcept {
  if (!AdjRibCommonUtils::isAfiNegotiated(
          prefix, groupKey_.afiIpv4Negotiated, groupKey_.afiIpv6Negotiated)) {
    XLOGF(
        DBG4,
        "Ignore RibWithdrawal of prefix {}: AFI is not supported for group {}.",
        folly::IPAddress::networkToString(prefix),
        groupDescriptor_);
    return;
  }

  XLOGF(
      DBG5,
      "Group {} processing withdrawal of {}",
      groupDescriptor_,
      folly::IPAddress::networkToString(prefix));

  // TODO: Out-delay support will be added in future diff
  // When implemented: check and remove from deferredUpdates_ here

  // Get group RIB-OUT entry via radix node iterator
  auto groupOwnerKey = getGroupOwnerKey();
  AdjRibEntry* adjRibEntry = nullptr;

  AdjRibPathTree::Iterator radixPathNodeItr;
  AdjRibLiteTree::Iterator radixLiteNodeItr;

  if (groupKey_.sendAddPath) {
    radixPathNodeItr = getRadixNodeItrFromPathTree(PathTree_, prefix);
    adjRibEntry =
        getAdjRibEntryFromPathNodeItr(radixPathNodeItr, groupOwnerKey, pathId);
  } else {
    radixLiteNodeItr = getRadixNodeItrFromLiteTree(LiteTree_, prefix);
    adjRibEntry =
        getAdjRibEntryFromLiteNodeItr(radixLiteNodeItr, groupOwnerKey);
  }

  if (!adjRibEntry || !adjRibEntry->getPreOut()) {
    XLOGF(
        DBG3,
        "Received withdraw of prefix {} without announcement for group {}",
        folly::IPAddress::networkToString(prefix),
        groupDescriptor_);
    return;
  }

  // Lazy clone BEFORE removal
  if (!detachedPeers_.empty()) {
    if (groupKey_.sendAddPath) {
      lazyClonePathForDetachedPeers(
          prefix, pathId, radixPathNodeItr, adjRibEntry);
    } else {
      lazyCloneLiteForDetachedPeers(
          prefix, pathId, radixLiteNodeItr, adjRibEntry);
    }
  }

  // Clear preOut and postOutPolicy (same as AdjRib)
  adjRibEntry->setPreOut(nullptr);
  adjRibEntry->setPostOutPolicy({});
  stats_.decrementPreOutPrefixCount(prefix.first.isV4());

  // Try to insert withdrawal into packing list
  // Only withdraw if we previously announced (has postAttr)
  auto oldPostAttr = adjRibEntry->getPostAttr();
  if (oldPostAttr) {
    XLOGF_IF(
        DBG1,
        stats_.getPostOutPrefixCount() == 0,
        "Invalid sent prefix count of {} for group {}",
        folly::IPAddress::networkToString(prefix),
        groupDescriptor_);
    stats_.decrementPostOutPrefixCount(
        prefix.first.isV4(), getNumInSyncPeers());

    adjRibEntry->setPostAttr(nullptr);
    auto prefixPathId = std::make_pair(prefix, pathId);

    // Add to packing list with nullptr key (marks as withdrawal)
    tryUpdateAttrToPrefixMapForGroup(prefixPathId, oldPostAttr, nullptr);

    XLOGF(
        DBG3,
        "Withdrawing {} from group {}",
        folly::IPAddress::networkToString(prefix),
        groupDescriptor_);
  } else {
    XLOGF(
        DBG3,
        "Ignoring withdraw of prefix {} for unannounced route for group {}",
        folly::IPAddress::networkToString(prefix),
        groupDescriptor_);
  }

  // Delete entry if no longer needed (neither preOut nor postAttr)
  if (!adjRibEntry->getPreOut() && !adjRibEntry->getPostAttr()) {
    if (groupKey_.sendAddPath) {
      deleteFromPathTree(
          PathTree_, std::move(radixPathNodeItr), groupOwnerKey, pathId);
    } else {
      deleteFromLiteTree(LiteTree_, std::move(radixLiteNodeItr), groupOwnerKey);
    }
  }
}

/*
 * @brief  Process RibOutAnnouncement for the group
 *         Builds group packing list from announcement entries
 *
 * Similar to AdjRib::processRibOutAnnouncement but at group level.
 * This processes each entry and builds the group's attrToPrefixMap_.
 *
 * @param  announcement - RibOutAnnouncement with routes to process
 *
 * @return void
 */
void AdjRibOutGroup::processRibOutAnnouncement(
    const RibOutAnnouncement& announcement) noexcept {
  // Same pattern as AdjRibOut.cpp: select entries based on sendAddPath flag
  const auto& entries = groupKey_.sendAddPath ? announcement.addPathEntries
                                              : announcement.entries;

  XLOGF(
      DBG1,
      "Group {} processing announcement with {} entries (sendAddPath={})",
      groupDescriptor_,
      entries.size(),
      groupKey_.sendAddPath);

  // Process selected entries
  for (const auto& entry : entries) {
    processRibAnnouncedEntryForGroup(entry);
  }

  /*
   * Transition to WAITING if we have work to do
   * State must be READY or IDLE to transition
   */
  if ((state_ == UpdateGroupState::READY || state_ == UpdateGroupState::IDLE) &&
      !attrToPrefixMap_.empty()) {
    state_ = UpdateGroupState::WAITING;
    XLOGF(
        DBG2,
        "Group {} transitioned to WAITING state with {} attr entries in packing list",
        groupDescriptor_,
        attrToPrefixMap_.size());
  }

  /*
   * Set egressEoRsPending_ before scheduling async send, mirroring
   * AdjRib::handleRibAnnouncedEntries which sets the flag at intake time.
   */
  if (announcement.sendWithEoR) {
    egressEoRsPending_ = true;
  }

  /*
   * Schedule async build and send - don't block the caller
   */
  asyncScope_.add(
      folly::coro::co_withExecutor(
          &evb_, buildAndSendGroupBgpMessages(announcement.sendWithEoR)));
}

/*
 * @brief  Group-level canAnnounce check
 *         Determines if route can be announced based on group's session type
 *
 * Key differences from peer-level canAnnounce():
 * - Skips same-peer check (split-horizon done per-peer during distribution)
 * - Uses groupKey_.sessionType and groupKey_.isRrClient
 * - Checks IBGP/EBGP rules that apply uniformly to all peers in group
 *
 * @param  update - The announcement entry to check
 * @return bool - true if route can be announced by this group
 */
bool AdjRibOutGroup::canAnnounceForGroup(
    const RibOutAnnouncementEntry& update) noexcept {
  // Skip same-peer check - that's done per-peer during distribution
  // because each peer has different peerAddr

  /*
   * RFC 1997 well-known community egress filter. Run BEFORE the IBGP
   * early-return below because NO_ADVERTISE MUST suppress on ANY session
   * type, including IBGP. NO_EXPORT / NO_EXPORT_SUBCONFED do not affect
   * IBGP per the filter's own session-type checks. Gated by gflag so
   * rollout is bisectable; when the flag is off behavior is unchanged.
   *
   * Shares the wrapper with the per-peer path (AdjRib::canAnnounceEntry)
   * so the decision + logging + counter side-effects stay in lock-step
   * regardless of whether update-grouping is enabled.
   */
  if (FLAGS_enable_well_known_community_filter &&
      applyWellKnownCommunityFilter(
          update.attrs,
          groupKey_.sessionType,
          groupDescriptor_,
          update.prefix)) {
    return false;
  }

  if (groupKey_.sessionType == BgpSessionType::IBGP) {
    if (groupKey_.isRrClient) {
      // RR clients can announce all routes
      XLOGF(
          DBG4,
          "Group {} permits announcement of {}. Reason: Group is RR client",
          groupDescriptor_,
          folly::IPAddress::networkToString(update.prefix));
      return true;
    }

    // Non-RR IBGP peers: only announce eBGP routes, local routes, or RRC routes
    // Local routes will have addr.isZero()
    if (update.peer.sessionType != BgpSessionType::IBGP ||
        update.peer.addr.isZero() || update.peer.isRrClient) {
      XLOGF(
          DBG4,
          "Group {} permits announcement of {}. "
          "Reason: EBGP learnt route | Local Route | RRC Learnt Route",
          groupDescriptor_,
          folly::IPAddress::networkToString(update.prefix));
      return true;
    }

    // IBGP-to-IBGP not allowed for non-RR peers
    XLOGF(
        DBG4,
        "Group {} blocks announcement of {}. Reason: Learnt from same AS",
        groupDescriptor_,
        folly::IPAddress::networkToString(update.prefix));
    return false;
  }

  // eBGP peers can announce all routes
  return true;
}

/*
 * @brief  Process a single RibOutAnnouncementEntry for the group
 *         Similar to AdjRib::processRibAnnouncedEntry but at group level
 *
 * Key differences from peer-level:
 * - Runs egress policy once for the group (all peers share same policy)
 * - Skips canAnnounce() checks (all peers in group have identical config)
 * - Stores in group-level RIB-OUT tree using getGroupOwnerKey()
 * - Skips per-peer LBW config (no per-peer config at group level)
 * - Skips nexthop validation (nexthop set per-peer at send time)
 * - No stats tracking (groups don't track stats like peers)
 *
 * @param  entry - The announcement entry to process
 *
 * @return void
 */
void AdjRibOutGroup::processRibAnnouncedEntryForGroup(
    const RibOutAnnouncementEntry& entry) noexcept {
  XLOGF(
      DBG5,
      "Group {} processing entry for {}",
      groupDescriptor_,
      folly::IPAddress::networkToString(entry.prefix));

  // ignore unsupported afi
  if (!AdjRibCommonUtils::isAfiNegotiated(
          entry.prefix,
          groupKey_.afiIpv4Negotiated,
          groupKey_.afiIpv6Negotiated)) {
    XLOGF(
        DBG4,
        "Ignore RibAnnouncement of prefix {}: AFI is not supported for group {}.",
        folly::IPAddress::networkToString(entry.prefix),
        groupDescriptor_);
    return;
  }

  // sanity check to determine if we should announce to the members in the group
  if (!canAnnounceForGroup(entry)) {
    XLOGF(
        DBG3,
        "Implicit withdrawing prefix {} from group {}.",
        folly::IPAddress::networkToString(entry.prefix),
        groupDescriptor_);

    processGroupRibWithdraw(entry.prefix, entry.pathIdToSend);
    return;
  }

  const std::string updatePeerIdStr =
      nettools::bgplib::BgpPeerId(entry.peer.addr, entry.peer.routerId).str();

  // Get or create adjRibEntry in group RIB-OUT tree
  auto adjRibEntry = tryInsertRibOutEntry(
      entry.prefix, entry.attrs->getNexthop(), entry.pathIdToSend);
  CHECK(adjRibEntry);

  // Store preOut attributes
  adjRibEntry->setPreOut(entry.attrs);
  adjRibEntry->setRibVersion(entry.ribVersion);

  // Clone attributes for policy evaluation
  // NOTE: Skip per-peer LBW config (no per-peer config at group level)
  auto prePolicyAttrs = entry.attrs->clone();
  if (entry.isPartialDrain) {
    applyPartialDrainCommunities(prePolicyAttrs);
  }

  // Get post policy attributes
  // Policy cache lookup happens inside getPostOutPolicyAttributesAndInfo
  const auto& [policyCachedAttrs, postPolicyInfo] =
      getPostOutPolicyAttributesAndInfo(
          entry, adjRibEntry, prePolicyAttrs, updatePeerIdStr);

  /* Store flag on AdjRibEntry for CLI display (AdjRibShow). */
  adjRibEntry->setNexthopSetByPolicy(postPolicyInfo.isNexthopSetByPolicy);

  if (!policyCachedAttrs) {
    // Policy blocked this prefix
    tryInsertWithdrawal(
        entry.prefix,
        adjRibEntry,
        fmt::format(
            "Group {} withdrawing {} from [{}]. "
            "Reason: Blocked prefix by policy. (Previously announced to group)",
            groupDescriptor_,
            folly::IPAddress::networkToString(entry.prefix),
            updatePeerIdStr),
        fmt::format(
            "Group {} ignoring Rib announcement {} from [{}]. "
            "Reason: Blocked prefix by policy. (Not previously announced to group)",
            groupDescriptor_,
            folly::IPAddress::networkToString(entry.prefix),
            updatePeerIdStr));
    return;
  }

  // NOTE: Skip nexthop validation at group level
  // Nexthop is set per-peer at send time, not at group level

  /*
   * Prefix is permitted from egress policy.
   * Attributes need to be updated before sending to packing list.
   *
   * NOTE: directly modifying prePolicyAttrs will make cache miss since it is
   * used as the policy cache key. Clone it for modification.
   */
  auto postOutAttrsNew = prePolicyAttrs->clone();
  updateAttributesOutWithoutNexthop(
      entry, policyCachedAttrs, postOutAttrsNew, postPolicyInfo);

  /* Attributes will not change any more. Publish. */
  postOutAttrsNew->publish();

  // Check if we announced the prefix before
  // post out is set if the prefix was previously announced
  if (adjRibEntry->getPostAttr()) {
    // Announce the prefix to group again only if postOut has changed
    // We are doing deep compare to avoid notifying group in cases where
    // due to policy changes of attributes, we end up with same contents
    // but different BgpPath shared_ptr
    if (*adjRibEntry->getPostAttr() == *postOutAttrsNew) {
      XLOGF(
          DBG3,
          "Group {} skipping unchanged announcement {} from [{}]. "
          "Reason: Previously announced to group",
          groupDescriptor_,
          folly::IPAddress::networkToString(entry.prefix),
          updatePeerIdStr);
      return;
    }
  } else {
    // First-time announcement — increment group prefix count
    stats_.incrementPostOutPrefixCount(
        entry.prefix.first.isV4(), getNumInSyncPeers());
  }

  XLOGF(
      DBG3,
      "Group {} announcing {} from [{}].",
      groupDescriptor_,
      folly::IPAddress::networkToString(entry.prefix),
      updatePeerIdStr);

  auto prefixPathId = std::make_pair(entry.prefix, adjRibEntry->getPathId());

  auto oldPostAttr = adjRibEntry->getPostAttr();

  // Set the post out
  // Majority of the cases we update attributes with nexthop/AS-Path changes
  // or policy changes, but in few cases where there is no change between
  // preOut and postOut, this deep compare will save memory.
  if (*postOutAttrsNew == *adjRibEntry->getPreOut()) {
    auto preOutPostAttr = adjRibEntry->getPreOut();
    adjRibEntry->setPostAttr(preOutPostAttr);
  } else {
    adjRibEntry->setPostAttr(postOutAttrsNew);
  }
  tryUpdateAttrToPrefixMapForGroup(
      prefixPathId,
      oldPostAttr,
      adjRibEntry->getPostAttr(),
      postPolicyInfo.isNexthopSetByPolicy);
}

AdjRibOutGroupConsumer::~AdjRibOutGroupConsumer() {
  adjRibOutGroup_.reset();
}

/*
 * @brief  Try to insert or get RIB-OUT entry for the group
 *         Similar to AdjRib::tryInsertRibOutEntry but operates at group level
 *
 * Key differences from peer-level:
 * - Uses group owner key instead of peer owner key
 * - Uses enableRibAllocatedPathId_ to decide pathId (no pathIdGenerator_)
 * - Conditionally uses PathTree or LiteTree based on groupKey_.sendAddPath
 *
 * @param  prefix - The prefix to insert/get
 * @param  nexthop - Nexthop (unused at group level, kept for signature compat)
 * @param  pathIdToSend - Path ID from RIB
 *
 * @return AdjRibEntry* - Pointer to entry (never null)
 */
AdjRibEntry* FOLLY_NULLABLE AdjRibOutGroup::tryInsertRibOutEntry(
    const folly::CIDRNetwork& prefix,
    const folly::IPAddress& /* nexthop */,
    const uint32_t pathIdToSend) noexcept {
  // Group level always uses RIB-allocated path ID (no pathIdGenerator)
  // Per spec: "use enableRibAllocatedPathId_, dont use pathIdGenerator_, always
  // use pathIdToSend"
  auto pathId = enableRibAllocatedPathId_ ? pathIdToSend : pathIdToSend;

  auto groupOwnerKey = getGroupOwnerKey();
  AdjRibEntry* adjRibEntry = nullptr;

  // Use PathTree or LiteTree based on UpdateGroupKey.sendAddPath setting
  if (groupKey_.sendAddPath) {
    auto radixPathNodeItr = getRadixNodeItrFromPathTree(PathTree_, prefix);
    adjRibEntry =
        getAdjRibEntryFromPathNodeItr(radixPathNodeItr, groupOwnerKey, pathId);
    if (!adjRibEntry) {
      XLOGF(
          DBG4,
          "Learning new prefix from Rib for group {}: {}",
          groupDescriptor_,
          folly::IPAddress::networkToString(prefix));
      stats_.incrementPreOutPrefixCount(prefix.first.isV4());
      return addToPathTree(PathTree_, prefix, groupOwnerKey, pathId);
    }
    // Lazy clone before the caller mutates this existing entry
    if (!detachedPeers_.empty()) {
      lazyClonePathForDetachedPeers(
          prefix, pathId, radixPathNodeItr, adjRibEntry);
    }
  } else {
    auto radixLiteNodeItr = getRadixNodeItrFromLiteTree(LiteTree_, prefix);
    adjRibEntry =
        getAdjRibEntryFromLiteNodeItr(radixLiteNodeItr, groupOwnerKey);
    if (!adjRibEntry) {
      XLOGF(
          DBG4,
          "Learning new prefix from Rib for group {}: {}",
          groupDescriptor_,
          folly::IPAddress::networkToString(prefix));
      stats_.incrementPreOutPrefixCount(prefix.first.isV4());
      return addToLiteTree(LiteTree_, prefix, groupOwnerKey, pathId);
    }
    // Lazy clone before the caller mutates this existing entry
    if (!detachedPeers_.empty()) {
      lazyCloneLiteForDetachedPeers(
          prefix, pathId, radixLiteNodeItr, adjRibEntry);
    }
  }

  XLOGF(
      DBG4,
      "Updating preOut attributes of prefix {} for group {}",
      folly::IPAddress::networkToString(prefix),
      groupDescriptor_);

  return adjRibEntry;
}

/*
 * @brief  Try to insert withdrawal for a prefix in the group's packing list
 *         Similar to AdjRib::tryInsertWithdrawal but operates at group level
 *
 * Key differences from peer-level:
 * - Skips stats tracking (no stats_.decrementPostOutPrefixCount)
 * - Uses tryUpdateAttrToPrefixMapForGroup instead of tryUpdateAttrToPrefixMap
 * - Logs with group name instead of peer name
 *
 * @param  prefix - The prefix to withdraw
 * @param  adjRibEntry - The AdjRibEntry for this prefix
 * @param  insertedMsg - Message to log if withdrawal is inserted
 * @param  notInsertedMsg - Message to log if withdrawal is not inserted
 *
 * @return void
 */
void AdjRibOutGroup::tryInsertWithdrawal(
    const folly::CIDRNetwork& prefix,
    AdjRibEntry* adjRibEntry,
    const std::string& insertedMsg,
    const std::string& notInsertedMsg) noexcept {
  auto oldPostAttr = adjRibEntry->getPostAttr();
  if (oldPostAttr) {
    // Withdraw the prefix only if we previously announced it to this group
    // This can happen if attributes change for a prefix and policy based on
    // new attributes blocks the prefix. Withdraw it from group.
    XLOGF_IF(
        DBG1,
        stats_.getPostOutPrefixCount() == 0,
        "Invalid sent prefix count of {} for group {}",
        folly::IPAddress::networkToString(prefix),
        groupDescriptor_);
    stats_.decrementPostOutPrefixCount(
        prefix.first.isV4(), getNumInSyncPeers());

    adjRibEntry->setPostAttr(nullptr);

    auto prefixPathId = std::make_pair(prefix, adjRibEntry->getPathId());

    // Update attrToPrefixMap to move prefixPathId from the previous postAttrs
    // to the new postAttr (nullptr).
    tryUpdateAttrToPrefixMapForGroup(
        prefixPathId, oldPostAttr, adjRibEntry->getPostAttr());

    XLOG(DBG3, insertedMsg);
  } else {
    // we didn't inform group before, so no need of informing to group now.
    XLOG(DBG3, notInsertedMsg);
  }
}

/*
 * @brief  Get post-policy attributes, policy term name, and policy info
 *         Similar to AdjRib::getPostPolicyAttributesPolicyTermAndInfo
 *
 * Key differences from peer-level:
 * - Uses group's policyManager_ and policyCache_
 * - Logs with group name instead of peer name
 * - Otherwise identical logic
 *
 * TODO: This function is exactly the same as in AdjRib.cpp. Consider
 * consolidating and moving to AdjRibCommon.
 *
 * @param  policyName - Name of the egress policy to evaluate
 * @param  prefix - The prefix being evaluated
 * @param  prePolicyAttrs - Attributes before policy evaluation
 * @param  policyActionData - Action data for policy (LBW, etc.)
 *
 * @return tuple of (postPolicyAttrs, policyTermName, PostPolicyInfo)
 */
std::tuple<std::shared_ptr<const BgpPath>, std::string, PostPolicyInfo>
AdjRibOutGroup::getPostPolicyAttributesPolicyTermAndInfo(
    const std::string& policyName,
    const folly::CIDRNetwork& prefix,
    const std::shared_ptr<const BgpPath>& prePolicyAttrs,
    const std::shared_ptr<BgpPolicyActionData>& policyActionData,
    bool isPartialDrain) noexcept {
  std::optional<AdjRibPolicyCache::PolicyCacheValue> cacheResult{std::nullopt};
  std::shared_ptr<const BgpPath> postPolicyAttrs{nullptr};
  std::string policyTermName;
  PostPolicyInfo postPolicyInfo;

  if (policyCache_) {
    cacheResult = policyCache_->lookupPolicyCache(
        policyName,
        policyManager_->getPolicyAttributesMask(policyName),
        prefix,
        prePolicyAttrs,
        policyActionData,
        isPartialDrain);
  }

  if (cacheResult == std::nullopt) {
    // Policy cache miss - evaluate policy
    XLOGF(
        DBG5,
        "Policy Cache Miss for {} {} for group {}",
        folly::IPAddress::networkToString(prefix),
        policyName,
        groupDescriptor_);

    // Clone attributes for policy evaluation
    auto prePolicyAttrsClone = prePolicyAttrs->clone();

    // Apply policy
    auto policyOut = policyManager_->applyPolicy(
        policyName,
        PolicyInMessage({prefix}, prePolicyAttrsClone, policyActionData));

    const auto attrsAndPolicy =
        getPostPolicyOutAttrsAndPolicyFromMessage(prefix, policyOut);

    postPolicyAttrs = attrsAndPolicy->attrs;
    policyTermName = attrsAndPolicy->policyName;
    if (policyActionData) {
      postPolicyInfo.isMedSetByPolicy = policyActionData->isMedSetByPolicy;
      postPolicyInfo.isNexthopSetByPolicy =
          policyActionData->isNexthopSetByPolicy;
    }

    if (policyCache_) {
      policyCache_->addToPolicyCache(
          policyName,
          policyManager_->getPolicyAttributesMask(policyName),
          prefix,
          prePolicyAttrs,
          policyActionData,
          attrsAndPolicy,
          isPartialDrain);
    }

  } else {
    // Policy cache hit
    XLOGF(
        DBG5,
        "Policy Cache Hit for {} {} for group {}",
        folly::IPAddress::networkToString(prefix),
        policyName,
        groupDescriptor_);

    if (cacheResult->attrsAndPolicy) {
      postPolicyAttrs = cacheResult->attrsAndPolicy->attrs;
      policyTermName = cacheResult->attrsAndPolicy->policyName;
    }
    postPolicyInfo.isMedSetByPolicy = cacheResult->isMedSetByPolicy;
    postPolicyInfo.isNexthopSetByPolicy = cacheResult->isNexthopSetByPolicy;
  }

  return {
      std::move(postPolicyAttrs),
      std::move(policyTermName),
      std::move(postPolicyInfo)};
}

/*
 * @brief  Get post-policy attributes and metadata for the group
 *         Similar to AdjRib::getPostOutPolicyAttributesAndInfo
 *
 * Key differences from peer-level:
 * - Uses group's policyManager, policyCache, and groupKey_.egressPolicyName
 * - Logs with group name instead of peer name
 * - No CRF (egress route filter) check at group level for now
 * - Otherwise identical logic
 *
 * @param  update - The RibOutAnnouncementEntry being processed
 * @param  adjRibEntry - The AdjRibEntry for this prefix
 * @param  prePolicyAttrs - Attributes before policy evaluation
 * @param  updatePeerIdStr - Source peer ID string (for logging)
 *
 * @return pair of (postPolicyAttrs, PostPolicyInfo)
 */
std::pair<const std::shared_ptr<const BgpPath>, const PostPolicyInfo>
AdjRibOutGroup::getPostOutPolicyAttributesAndInfo(
    const RibOutAnnouncementEntry& update,
    AdjRibEntry* adjRibEntry,
    const std::shared_ptr<const BgpPath>& prePolicyAttrs,
    const std::string& /* updatePeerIdStr */) noexcept {
  std::shared_ptr<const BgpPath> policyResultAttrs{nullptr};
  PostPolicyInfo postPolicyResultInfo;

  // adjRibEntry must not be nullptr
  CHECK(adjRibEntry != nullptr);

  if (egressPolicyConfigured()) {
    auto policyActionData = std::make_shared<BgpPolicyActionData>();

    const auto& [attrs, postTermName, postPolicyInfo] =
        getPostPolicyAttributesPolicyTermAndInfo(
            *groupKey_.egressPolicyName,
            update.prefix,
            prePolicyAttrs,
            policyActionData,
            update.isPartialDrain);

    policyResultAttrs = attrs;
    postPolicyResultInfo = postPolicyInfo;
    adjRibEntry->setPostOutPolicy(postTermName);
  } else {
    policyResultAttrs = prePolicyAttrs;
  }

  // TODO: Add egress route filter (CRF) check for group level if needed
  // For now, skip CRF check at group level

  return {policyResultAttrs, postPolicyResultInfo};
}

/*
 * @brief  Update all attributes except nexthop before putting in packing list
 *         Uses the common implementation shared with per-peer AdjRib
 *
 * Key differences from peer-level:
 * - Checks that peeringParams_ is cached before using it
 * - Otherwise delegates to common implementation
 *
 * @param  update - The RibOutAnnouncementEntry being processed
 * @param  policyResultAttrs - Attributes after policy evaluation
 * @param  attrsToUpdate - Attributes to modify (must not be published)
 * @param  postPolicyInfo - Metadata from policy evaluation
 *
 * @return void
 */
void AdjRibOutGroup::updateAttributesOutWithoutNexthop(
    const RibOutAnnouncementEntry& update,
    const std::shared_ptr<const BgpPath>& policyResultAttrs,
    std::shared_ptr<BgpPath> attrsToUpdate,
    const PostPolicyInfo& postPolicyInfo) noexcept {
  if (!peeringParams_) {
    XLOGF(WARN, "No peering params cached for group {}", groupDescriptor_);
    return;
  }

  PeerConfig config{
      *peeringParams_, groupKey_.egressPolicyName, policyManager_.get()};
  updateAttributesOutWithoutNexthopCommon(
      config, update, policyResultAttrs, attrsToUpdate, postPolicyInfo);
}

/*
 * @brief  Pack prefixes into BgpUpdate2 thrift collection.
 *         Wrapper for packPrefixesCommon that uses group-specific
 * parameters.
 *
 * @param  prefixPathIds - Set of (prefix, pathId) pairs (modified in place)
 * @param  bgpUpdatePrefixes - Container to pack prefixes into
 * @param  sendAddPath - Whether to include path IDs in packed prefixes
 * @return Number of prefixes packed
 */
uint32_t AdjRibOutGroup::packGroupPrefixes(
    PrefixSet& prefixPathIds,
    std::vector<nettools::bgplib::RiggedIPPrefix>& bgpUpdatePrefixes,
    bool sendAddPath) noexcept {
  return packPrefixesCommon(
      prefixPathIds, bgpUpdatePrefixes, sendAddPath, groupDescriptor_);
}

/*
 * @brief  Build a single BGP UPDATE message from attrToPrefixMap entry
 *         Similar to AdjRib::buildAndQueueAnnouncements.
 *
 * Handles BOTH announcements and withdrawals in one method.
 * Drains the entire pfxSet.
 *
 * Key differences from peer-level:
 * - Uses pre-calculated nexthop from processRibAnnouncedEntryForGroup
 * - No stats tracking (groups don't have stats_ member)
 * - Uses groupKey_.sendAddPath instead of member variable
 *
 * @param  attrsWithAfi - Pair of (attributes, AFI) from map key
 * @param  prefixPathIds - Set of prefixes to pack (cleared in place after
 * draining)
 * @return BgpUpdate2 message, or nullptr if nothing to pack
 */
std::shared_ptr<nettools::bgplib::BgpUpdate2> AdjRibOutGroup::buildGroupUpdate(
    const BgpPathWithAfi& attrsWithAfi,
    PrefixSet& prefixPathIds) noexcept {
  if (prefixPathIds.empty()) {
    return nullptr;
  }

  auto& postOutAttrs = attrsWithAfi.attrs;
  auto afi = attrsWithAfi.afi;
  std::shared_ptr<nettools::bgplib::BgpUpdate2> update;

  if (postOutAttrs) {
    /* Case 1: Announcement */
    update = postOutAttrs->getBgpUpdate2();
    update->mpAnnounced()->afi() = afi;
    update->mpAnnounced()->safi() =
        nettools::bgplib::BgpUpdateSafi::SAFI_UNICAST;
    update->mpAnnounced()->nexthop() =
        network::toBinaryAddress(postOutAttrs->getNexthop());

    packGroupPrefixes(
        prefixPathIds,
        *update->mpAnnounced()->prefixes(),
        groupKey_.sendAddPath);

  } else {
    /* Case 2: Withdrawal */
    update = std::make_shared<nettools::bgplib::BgpUpdate2>();
    if (afi == nettools::bgplib::BgpUpdateAfi::AFI_IPv4) {
      packGroupPrefixes(
          prefixPathIds, *update->v4Withdrawn2(), groupKey_.sendAddPath);
    } else if (afi == nettools::bgplib::BgpUpdateAfi::AFI_IPv6) {
      packGroupPrefixes(
          prefixPathIds,
          *update->mpWithdrawn()->prefixes(),
          groupKey_.sendAddPath);
      update->mpWithdrawn()->afi() = nettools::bgplib::BgpUpdateAfi::AFI_IPv6;
      update->mpWithdrawn()->safi() =
          nettools::bgplib::BgpUpdateSafi::SAFI_UNICAST;
    }
  }

  return update;
}

/*
 * @brief  Build group BGP messages from packing list
 *         Similar to AdjRib::sendBgpUpdates but for update groups
 *
 * Orchestrates the complete message building and distribution workflow:
 * 1. Iterates through attrToPrefixMap_ entries
 * 2. Builds one BGP UPDATE at a time.
 * 3. Distributes message to all in-sync peers
 * 4. Removes processed prefixes from packing list
 * 5. Repeats until packing list is empty
 *
 * Key differences from peer-level (AdjRib::sendBgpUpdates):
 * - Synchronous (no co_await) - runs in single event loop iteration
 * - No stats tracking (groups don't have stats_ member)
 * - Distributes one message to N peers instead of N messages
 * - EoR notification deferred to later implementation
 *
 * @param  sendWithEoR - Whether to notify peers to send EoR (later feature)
 * @return void
 */
folly::coro::Task<void> AdjRibOutGroup::buildAndSendGroupBgpMessages(
    bool sendWithEoR) noexcept {
  ScopedProfile profile("AdjRibOutGroup::buildAndSendGroupBgpMessages");
  /*
   * Guard against re-entrant calls to maintain BGP UPDATE ordering
   * If already building/sending, skip this invocation to avoid:
   * 1. Message ordering violations (UPDATE #2 sent before UPDATE #1 completes)
   * 2. Concurrent modification of attrToPrefixMap_ during iteration
   * 3. Multiple coroutines waiting on same blocked peers
   */
  if (packingInProgress_) {
    XLOGF(
        DBG3,
        "Group {} packing already in progress, skipping duplicate invocation",
        groupDescriptor_);
    co_return;
  }

  packingInProgress_ = true;

  /* Check for asyncScope cancellation at start */
  co_await folly::coro::co_safe_point;

  /*
   * Cancel changeListConsumeTimer_ to prevent concurrent modifications
   * of attrToPrefixMap_ while we're iterating it.
   * Timer will be rescheduled when we exit.
   */
  if (changeListConsumeTimer_) {
    changeListConsumeTimer_->cancelTimeout();
  }

  /*
   * RAII guard to ensure cleanup even if exception occurs:
   * 1. Clear packingInProgress_ flag
   * 2. Reschedule changeListConsumeTimer_
   * 3. Transition group to IDLE if packing list is empty.
   * 4. Check and accept all ready to join peers.
   */
  auto guard = folly::makeGuard([this]() {
    packingInProgress_ = false;
    if (changeListConsumeTimer_) {
      changeListConsumeTimer_->scheduleTimeout(
          std::chrono::milliseconds(mraiInterval_));
    }

    /*
     * Transition to IDLE if packing list is now empty.
     */
    if (attrToPrefixMap_.empty()) {
      XLOGF_IF(
          DBG2,
          state_ != UpdateGroupState::IDLE,
          "Group {}: State Transition: {} -> IDLE",
          groupDescriptor_,
          state_);
      state_ = UpdateGroupState::IDLE;

      // Check for peers ready to rejoin after PL drain or PL empty.
      checkAndAcceptReadyToJoinPeers();
    }
  });

  // Return early if nothing to announce/withdraw and no EoR pending.
  if (attrToPrefixMap_.empty() && !sendWithEoR) {
    co_return;
  }

  XLOGF(
      DBG1,
      "Group {} building BGP messages from packing list ({} attr entries)",
      groupDescriptor_,
      attrToPrefixMap_.size());

  uint32_t msgCount = 0;
  uint32_t withdrawPrefixCnt = 0;
  uint32_t announcePrefixCnt = 0;

  // Iterate attrToPrefixMap and build/distribute one message at a time
  // Similar to AdjRib::sendBgpUpdates() but synchronous (no co_await)
  while (!attrToPrefixMap_.empty()) {
    /* Check for cancellation at start of each iteration */
    co_await folly::coro::co_safe_point;

    /*
     * If we were backpressured and resume here, it is currently possible for
     * the map to be cleared if the peer session flaps.
     * Here we need to check one more time to make sure that the map is not
     * empty before taking the begin iterator.
     */
    if (attrToPrefixMap_.empty()) {
      break;
    }

    auto it = attrToPrefixMap_.begin();
    auto& [attrWithAfi, pfxSet] = *it;
    const auto attr = attrWithAfi.attrs;
    const auto afi = attrWithAfi.afi;
    const auto isNhSetByPolicy = attrWithAfi.isNexthopSetByPolicy;

    // Build one message (announcement or withdrawal) with size limit
    auto update = buildGroupUpdate(attrWithAfi, pfxSet);

    /*
     * Remove entry from map if all prefixes processed.
     * This step is done above the co_await in case the iterator becomes
     * invalidated after the coroutine yields.
     */
    if (pfxSet.empty()) {
      attrToPrefixMap_.erase(it);
    }

    if (update) {
      msgCount++;
      // Update counters
      if (attr) {
        // Announcement
        announcePrefixCnt += update->mpAnnounced()->prefixes()->size();
      } else {
        // Withdrawal
        if (afi == nettools::bgplib::BgpUpdateAfi::AFI_IPv4) {
          withdrawPrefixCnt += update->v4Withdrawn2()->size();
        } else if (afi == nettools::bgplib::BgpUpdateAfi::AFI_IPv6) {
          withdrawPrefixCnt += update->mpWithdrawn()->prefixes()->size();
        }
      }

      // Distribute to all in-sync peers
      // This suspends if any peer queue is full (backpressure)
      co_await distributeMessageToInSyncPeers(
          update, attr, afi, sendWithEoR, isNhSetByPolicy);
    }
  }

  /*
   * EoR handling after packing list drain, mirroring AdjRib::sendBgpUpdates:
   *   if (egressEoRsPending_) { sendPendingEoRs(); }
   *
   * 1. Notify in-sync peers to set their per-peer egressEoRsPending_ flags
   * 2. Call sendPendingEoRs() on each peer to actually send EoR markers
   * 3. Clear group egressEoRsPending_ flag
   */
  if (egressEoRsPending_) {
    notifyPeersToSendEoR();

    XLOGF(
        INFO,
        "Group {} processing pending EoRs for in-sync peers",
        groupDescriptor_);
    for (const auto& [bitPos, adjRib] : bitToAdjRibs_) {
      if (!BitmapUtils::isBitSet(adjRibSyncBitmap_, bitPos)) {
        continue;
      }
      if (!adjRib) {
        XLOGF(
            WARN,
            "Group {} has null AdjRib at bit {} when sending EoRs",
            groupDescriptor_,
            bitPos);
        continue;
      }
      co_await adjRib->sendPendingEoRs();
    }
    egressEoRsPending_ = false;
    XLOGF(INFO, "Group {} completed sending pending EoRs", groupDescriptor_);
  }

  XLOGF(
      INFO,
      "Group {} built and distributed {} BGP messages ({} withdrawals, {} announcements)",
      groupDescriptor_,
      msgCount,
      withdrawPrefixCnt,
      announcePrefixCnt);

  // packingInProgress_ flag will be cleared automatically by RAII guard
}

/*
 * @brief  Notify all in-sync peers to send their own EoR markers
 *
 * Lightweight EoR notification approach:
 * - Groups don't build/distribute EoR PDUs
 * - Instead, set egressEoRsPending flag on each in-sync peer
 * - Peers send their own EoRs via existing sendPendingEoRs() logic
 * - Handles per-peer AFI negotiation and backpressure automatically
 *
 * @return void
 */
void AdjRibOutGroup::notifyPeersToSendEoR() noexcept {
  uint32_t notifiedCount = 0;

  // Iterate through actual map keys, not range 0..size()
  for (const auto& [bitPos, adjRib] : bitToAdjRibs_) {
    if (!BitmapUtils::isBitSet(adjRibSyncBitmap_, bitPos)) {
      continue;
    }

    if (!adjRib) {
      XLOGF(
          WARN,
          "Group {} has null AdjRib at bit {} for EoR notification",
          groupDescriptor_,
          bitPos);
      continue;
    }

    // Set the peer's egressEoRsPending flag
    // Peer will send EoRs via its existing sendPendingEoRs() logic
    adjRib->setEgressEoRsPending();
    notifiedCount++;

    XLOGF(
        DBG3,
        "Group {} notified peer {} at bit {} to send EoR",
        groupDescriptor_,
        adjRib->getPeerName(),
        bitPos);
  }

  XLOGF(
      INFO,
      "Group {} notified {} in-sync peers to send EoR markers",
      groupDescriptor_,
      notifiedCount);
}

/*
 * @brief Distribute BGP UPDATE message to all in-sync peers
 * @param  message - BGP UPDATE message to distribute
 * @param  postOutAttrs - Post-policy BGP path attributes for nexthop
 * computation
 * @param  afi - Address Family Identifier (IPv4 or IPv6)
 * @return void
 */
folly::coro::Task<void> AdjRibOutGroup::distributeMessageToInSyncPeers(
    const std::shared_ptr<nettools::bgplib::BgpUpdate2>& message,
    const std::shared_ptr<const BgpPath>& postOutAttrs,
    nettools::bgplib::BgpUpdateAfi afi,
    bool sendWithEoR,
    bool isNexthopSetByPolicy) noexcept {
  if (!message) {
    XLOGF(
        WARN, "Group {} received null message to distribute", groupDescriptor_);
    co_return;
  }

  uint32_t pushOkCount = 0;
  uint32_t pushPendingCount = 0;
  uint32_t pushDroppedCount = 0;

  // Serialize once at group level if enabled
  nettools::bgplib::UpdateDescriptor groupDescriptor;
  if (updateGroupConfig_.enableSerializeGroupPdu) {
    bool as4byte = groupKey_.as4ByteCapable;
    bool extNhEncoding = groupKey_.extNhEncodingCapable;

    groupDescriptor = AdjRibGroupSerializer::serializeUpdateAndCreateDescriptor(
        *message, as4byte, extNhEncoding);

    if (!groupDescriptor.serializedGroupPDU) {
      XLOGF(
          ERR,
          "Group {} failed to serialize BGP UPDATE, skipping distribution",
          groupDescriptor_);
      co_return;
    }

    XLOGF(
        DBG4,
        "Group {} using zero-copy distribution with UpdateDescriptor",
        groupDescriptor_);
  }

  // Iterate through all in-sync peers
  for (const auto& [bitPos, adjRib] : bitToAdjRibs_) {
    if (!BitmapUtils::isBitSet(adjRibSyncBitmap_, bitPos)) {
      continue;
    }

    if (!adjRib) {
      XLOGF(
          WARN,
          "Group {} has null AdjRib at bit position {}",
          groupDescriptor_,
          bitPos);
      continue;
    }

    // Determine what to push to this peer
    nettools::bgplib::FiberBgpPeer::InputMessageT peerMessage;

    if (updateGroupConfig_.enableSerializeGroupPdu) {
      // Zero-copy path: use serialized UpdateDescriptor
      if (postOutAttrs) {
        /* Announcement: compute per-peer nexthop */
        auto peerNexthop = adjRib->getNewNexthopFromAttributesOut(
            afi == nettools::bgplib::BgpUpdateAfi::AFI_IPv4 /* isV4 */,
            postOutAttrs,
            isNexthopSetByPolicy);

        nettools::bgplib::UpdateDescriptor peerDescriptor = groupDescriptor;
        /* Set the appropriate nexthop based on address family */
        if (peerNexthop.isV4()) {
          peerDescriptor.v4Nexthop = peerNexthop;
        } else {
          peerDescriptor.v6Nexthop = peerNexthop;
        }
        peerMessage = peerDescriptor;
      } else {
        /* Withdrawal: nexthop irrelevant, use group descriptor as-is */
        peerMessage = groupDescriptor;
      }
    } else {
      /* Non-zero-copy path: clone BgpUpdate2 and set per-peer nexthop */
      if (postOutAttrs) {
        /* Announcement: clone message and set per-peer nexthop */
        auto clonedMessage =
            std::make_shared<nettools::bgplib::BgpUpdate2>(*message);

        /* Compute per-peer nexthop */
        auto peerNexthop = adjRib->getNewNexthopFromAttributesOut(
            afi == nettools::bgplib::BgpUpdateAfi::AFI_IPv4 /* isV4 */,
            postOutAttrs,
            isNexthopSetByPolicy);

        /* Set per-peer nexthop in the cloned message */
        clonedMessage->mpAnnounced()->nexthop() =
            network::toBinaryAddress(peerNexthop);
        clonedMessage->attrs()->nexthop() = peerNexthop.str();

        peerMessage = clonedMessage;
      } else {
        /* Withdrawal: nexthop irrelevant, use message as-is */
        peerMessage = message;
      }
    }

    // Try to push to this peer
    auto result = tryPushToPeer(peerMessage, adjRib, bitPos, sendWithEoR);

    if (result == PushResult::PUSH_OK) {
      pushOkCount++;
    } else if (result == PushResult::PUSH_PENDING) {
      pushPendingCount++;
    } else {
      /* PUSH_FAILED: scheduling failed, blocked state already cleared.
       * Message is dropped — peer session is likely going down. */
      pushDroppedCount++;
      XLOGF(
          WARN,
          "Group {}: Dropped message for peer {} at bit {} — "
          "deferred push scheduling failed",
          groupDescriptor_,
          adjRib->getPeerName(),
          bitPos);
    }
  }

  XLOGF(
      DBG3,
      "Group {} distributed message ({} immediate, {} pending, {} dropped)",
      groupDescriptor_,
      pushOkCount,
      pushPendingCount,
      pushDroppedCount);

  // Wait for all pending pushes to complete
  if (pushPendingCount > 0) {
    co_await waitForAllPendingPushes();

    XLOGF(DBG3, "Group {} all pending pushes completed", groupDescriptor_);
  }
}

/*
 * @brief  Try to push message to a peer's bounded queue
 *
 * Checks if peer's queue is blocked. If not, pushes immediately.
 * If blocked, schedules a deferred push coroutine and returns PUSH_PENDING.
 *
 * @param  message - Message to push (InputMessageT variant)
 * @param  adjRib - Peer's AdjRib
 * @param  bitPos - Peer's bit position in bitmap
 * @return PUSH_OK if pushed, PUSH_PENDING if deferred
 */
AdjRibOutGroup::PushResult AdjRibOutGroup::tryPushToPeer(
    const nettools::bgplib::FiberBgpPeer::InputMessageT& message,
    const std::shared_ptr<AdjRib>& adjRib,
    uint64_t bitPos,
    bool sendWithEoR) noexcept {
  auto boundedQueue = adjRib->getBoundedAdjRibOutQueue();
  if (!boundedQueue) {
    XLOGF(
        WARN,
        "Group {} peer {} at bit {} has null bounded queue",
        groupDescriptor_,
        adjRib->getPeerName(),
        bitPos);
    return PushResult::PUSH_OK; // Consider it done to avoid blocking group
  }

  /* Check if queue is blocked */
  if (boundedQueue->isBlocked()) {
    /*
     * Queue full - mark peer blocked (sets bitmap, checks slow peer criteria)
     */
    markPeerBlocked(adjRib, sendWithEoR);

    XLOGF(
        DBG3,
        "Group {} peer {} at bit {} blocked, scheduling deferred push",
        groupDescriptor_,
        adjRib->getPeerName(),
        bitPos);

    // Schedule deferred push on the peer's asyncScope
    if (!adjRib->scheduleDeferredPushToPeer(message)) {
      /* Scheduling failed (asyncScope null or cancelled). Clear the
       * blocked state we just set so waitForAllPushesToComplete doesn't
       * hang on a bit that will never be cleared. */
      XLOGF(
          WARN,
          "Group {}: Failed to schedule deferred push for peer {} at bit {}, "
          "clearing blocked state",
          groupDescriptor_,
          adjRib->getPeerName(),
          bitPos);
      markPeerUnblocked(adjRib);
      return PushResult::PUSH_FAILED;
    }

    return PushResult::PUSH_PENDING;
  } else {
    // Queue has space - push immediately
    boundedQueue->push(message);

    XLOGF(
        DBG5,
        "Group {} pushed message to peer {} at bit {} immediately",
        groupDescriptor_,
        adjRib->getPeerName(),
        bitPos);

    return PushResult::PUSH_OK;
  }
}

/*
 * @brief  Wait for all pending pushes to complete (all bits clear)
 *
 * Suspends the coroutine and periodically checks if all blocked bits are
 * cleared. Each bit is cleared by its corresponding deferredPushToPeer()
 * coroutine when that peer's push completes.
 *
 * Key improvement: This suspends the coroutine (allowing other groups and
 * coroutines to run) instead of blocking the entire EventBase thread.
 *
 * BUSY-WAIT JUSTIFICATION:
 * This function uses co_reschedule_on_current_executor without explicit sleep,
 * which may appear to be a busy-wait loop. However, this is acceptable because:
 * 1. Single-threaded EventBase: All coroutines run on the same event loop
 * 2. Cooperative multitasking: When we yield, the EventBase schedules other
 *    ready coroutines (including deferredPushToPeer) to run
 * 3. Progress guarantee: deferredPushToPeer coroutines will make progress
 *    (waiting on queue space, pushing, clearing bits) before we're rescheduled
 * 4. Not spinning: Between yields, other work runs on the EventBase. We don't
 *    consume CPU while suspended - the EventBase scheduler handles resume
 * 5. No sleep needed: Adding sleep would only add latency. The bits will be
 *    cleared as soon as deferredPushToPeer coroutines complete their pushes
 *
 * This pattern is equivalent to: while(!condition) { co_await other_work; }
 * where other_work is implicitly "run other ready coroutines on this executor".
 *
 * @return void
 */
folly::coro::Task<void> AdjRibOutGroup::waitForAllPendingPushes() noexcept {
  /* Check for asyncScope cancellation at start */
  co_await folly::coro::co_safe_point;

  /* Wait until all blocked bits are cleared by yielding to other coroutines.
   * Each yield allows deferredPushToPeer coroutines to run and clear bits.
   */
  while (!BitmapUtils::isAllBitsCleared(adjRibBlockedBitmap_)) {
    /* Check for cancellation at start of each iteration */
    co_await folly::coro::co_safe_point;

    /* Yield to allow deferred push coroutines to run and clear bits.
     * This is NOT a busy-wait - see function comment for justification.
     */
    co_await folly::coro::co_reschedule_on_current_executor;
  }

  XLOGF(
      DBG4,
      "Group {} all pending pushes completed, all bits clear",
      groupDescriptor_);
}

void AdjRibOutGroup::setBitToAdjRibForTesting(
    uint64_t bitPos,
    std::shared_ptr<AdjRib> adjRib) noexcept {
  bitToAdjRibs_[bitPos] = adjRib;
  if (adjRib) {
    adjRib->setGroupBitPosition(bitPos);
  }
}

bool AdjRibOutGroup::containsBitToAdjRibForTesting(
    uint64_t bitPos) const noexcept {
  return bitToAdjRibs_.find(bitPos) != bitToAdjRibs_.end();
}

/*
 * Register a peer with this update group
 * Assigns bit position and updates tracking structures
 * Handles different states based on whether group is initialized
 */
void AdjRibOutGroup::registerPeer(const std::shared_ptr<AdjRib>& adjRib) {
  if (!adjRib) {
    XLOGF(
        WARN,
        "Group {} received null adjRib in registerPeer",
        groupDescriptor_);
    return;
  }

  /*
   * Assign bit position using ConsumerBitManager
   * This properly handles bit reuse when peers leave and rejoin
   */
  uint64_t bit = bitManager_.getConsumerBit();

  XLOGF(
      INFO,
      "Group {}: Registering peer {} at bit position {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);

  // Store the mapping
  bitToAdjRibs_[bit] = adjRib;

  // Cache bit position in AdjRib for efficient lookups
  adjRib->setGroupBitPosition(bit);

  // Cache peering params from first peer
  if (!peeringParams_.has_value()) {
    peeringParams_ = adjRib->getPeeringParams();
    XLOGF(
        DBG2,
        "Group {}: Cached peering params from first peer {} at bit {}",
        groupDescriptor_,
        adjRib->getPeerName(),
        bit);
  }

  // Set bit in established bitmap
  BitmapUtils::setBit(adjRibEstablishedBitmap_, bit);

  // Determine peer state based on group state
  if (state_ == UpdateGroupState::UNINITIALIZED) {
    /*
     * Group not yet initialized - peer joins and waits for initial dump
     * Mark as in-sync immediately so peer receives dump when it happens
     */
    XLOGF(
        DBG1,
        "Group {}: Peer {} at bit {} State Transition: {} -> {}",
        groupDescriptor_,
        adjRib->getPeerName(),
        bit,
        adjRib->getPeerState(),
        PeerUpdateState::INIT);
    adjRib->setPeerState(PeerUpdateState::INIT);

    /*
     * Reset stale per-peer CL consumer set during AdjRib construction.
     * It was never registered with the ChangeTracker and would block
     * registerDetachedConsumer() on first detach.
     */
    adjRib->resetChangeListConsumer();

    /* Set in-sync bitmap bit - peer is ready to receive group dump */
    setSyncBit(bit);
  } else {
    // Group already initialized and running
    // New peer must catch up independently before joining
    detachedPeers_.insert(adjRib);

    XLOGF(
        DBG1,
        "Group {}: Peer {} at bit {} State Transition: {} -> {}",
        groupDescriptor_,
        adjRib->getPeerName(),
        bit,
        adjRib->getPeerState(),
        PeerUpdateState::DETACHED_INIT_DUMP);
    adjRib->setPeerState(PeerUpdateState::DETACHED_INIT_DUMP);
    adjRib->setAdjRibFlag(AdjRib::WAS_DETACHED_INIT_DUMP_PEER);

    // Schedule individual initial dump for this peer
    // This will be done by PeerManager after registerPeer returns
  }
}

/*
 * Unregister a peer from this update group
 * Cleans up tracking structures and bitmaps
 */
void AdjRibOutGroup::unregisterPeer(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  if (!adjRib) {
    XLOGF(
        WARN,
        "Group {} received null adjRib in unregisterPeer",
        groupDescriptor_);
    return;
  }

  // Get bit position from adjRib
  uint64_t bit = adjRib->getGroupBitPosition();
  XLOGF(
      DBG1,
      "Group {}: Peer {} at bit {} State Transition: {} -> {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit,
      adjRib->getPeerState(),
      PeerUpdateState::DOWN);
  adjRib->setPeerState(PeerUpdateState::DOWN);

  /*
   * Cancel any pending deferred push coroutines for this peer
   * Note: asyncScope_ manages all group coroutines, cannot selectively cancel
   * per-peer. Clearing adjRibBlockedBitmap_ bit is sufficient - deferred push
   * coroutines hold shared_ptr<AdjRib> and will handle cleanup gracefully.
   * When coroutine wakes up, it will find bit cleared and return safely.
   */

  if (adjRib->isDetachedPeer()) {
    adjRib->deactivateDetachedModeProcessing();
  }

  adjRib->resetPeerBlockInfo();
  removePeer(adjRib);

  XLOGF(
      INFO,
      "Group {}: Successfully unregistered peer {} from bit {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);

  // TODO: 1. Handle last peer in a group going down.
  // TODO: 2. Handle last SYNC peer in a group going down.
}

/*
 * Handle detached peer termination
 * Called when a peer in DETACHED state goes down
 *
 * Detached peers operate independently like AdjRib without update groups.
 * Normal AdjRib cleanup (handled by terminateSession() after this call):
 *   - Change list consumer deactivation
 *   - Packing list cleanup (stored in AdjRib)
 *   - MRAI timer cancellation
 *
 * This function handles UPDATE GROUP SPECIFIC cleanup only:
 *   - Clear peer's bit from group bitmaps
 *   - Remove from group's tracking maps
 *   - Delete per-peer RIB-OUT entries from group's trees
 *   - Clear bit position association
 */
void AdjRibOutGroup::handleDetachedPeerDown(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  uint64_t bit = adjRib->getGroupBitPosition();
  XLOGF(
      DBG1,
      "Group {}: Peer {} at bit {} State Transition: {} -> {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit,
      adjRib->getPeerState(),
      PeerUpdateState::DOWN);
  adjRib->setPeerState(PeerUpdateState::DOWN);

  /*
   * Clean up detached mode state (CL consumer, timers, RIB version)
   */
  adjRib->deactivateDetachedModeProcessing();

  /*
   * Delete per-peer RIB-OUT entries from GROUP's trees
   * When a peer detaches, its RIB-OUT entries are cloned to use peer owner key
   * These cloned entries live in the GROUP's PathTree_/LiteTree_, not AdjRib
   * We must delete them here since they're group-owned data
   */
  auto peerOwnerKey = adjRib->getPeerOwnerKey();
  size_t deletedCount = 0;

  if (adjRib->sendAddPath()) {
    /* Clean up PathTree entries with peer owner key */
    for (auto itr = PathTree_.begin(); itr != PathTree_.end(); ++itr) {
      auto& ownerMap = itr->value();
      auto erased = ownerMap.erase(peerOwnerKey);
      if (erased > 0) {
        deletedCount++;
      }
    }

    XLOGF(
        INFO,
        "Group {}: Deleted {} PathTree entries for detached peer {} at bit {}",
        groupDescriptor_,
        deletedCount,
        adjRib->getPeerName(),
        bit);
  } else {
    /* Clean up LiteTree entries with peer owner key */
    for (auto itr = LiteTree_.begin(); itr != LiteTree_.end(); ++itr) {
      auto& ownerMap = itr->value();
      auto erased = ownerMap.erase(peerOwnerKey);
      if (erased > 0) {
        deletedCount++;
      }
    }

    XLOGF(
        INFO,
        "Group {}: Deleted {} LiteTree entries for detached peer {} at bit {}",
        groupDescriptor_,
        deletedCount,
        adjRib->getPeerName(),
        bit);
  }

  adjRib->resetPeerBlockInfo();
  removePeer(adjRib);

  XLOGF(
      INFO,
      "Group {}: Successfully cleaned up detached peer {} from bit {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);

  /*
   * Note: We don't call maybeDestroyUpdateGroup() here because:
   * 1. terminateSession() doesn't call it for detached peers
   * 2. The group might still have other members
   * 3. TODO: If this was the last member, the group will be destroyed
   * when the last non-detached peer leaves
   */
}

void AdjRibOutGroup::removePeer(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  uint64_t bit = adjRib->getGroupBitPosition();

  adjRib->resetSlowPeerDurationTimer();

  BitmapUtils::clearBit(adjRibEstablishedBitmap_, bit);
  clearSyncBit(bit);
  BitmapUtils::clearBit(adjRibBlockedBitmap_, bit);
  detachedPeers_.erase(adjRib);

  bitToAdjRibs_.erase(bit);
  bitManager_.freeConsumerBit(bit);
  adjRib->clearGroupBitPosition();
}

/*
 * Mark a peer as blocked due to TCP backpressure.
 * Sets bitmap bit, checks frequency threshold, schedules duration timer.
 */
void AdjRibOutGroup::markPeerBlocked(
    const std::shared_ptr<AdjRib>& adjRib,
    bool sendWithEoR) noexcept {
  uint64_t bitPos = adjRib->getGroupBitPosition();

  BitmapUtils::setBit(adjRibBlockedBitmap_, bitPos);

  // Transition peer state: JOINED_RUNNING -> JOINED_BLOCKED
  XLOGF(
      DBG1,
      "Group {}: Peer {} at bit {} State Transition: {} -> {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bitPos,
      adjRib->getPeerState(),
      PeerUpdateState::JOINED_BLOCKED);
  adjRib->setPeerState(PeerUpdateState::JOINED_BLOCKED);

  // Frequency-based detection: fixed window with counter reset
  auto now = std::chrono::steady_clock::now();
  auto& info = adjRib->getPeerBlockInfo();
  auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      now - info.windowStart);
  if (elapsed >= updateGroupConfig_.slowPeerBlockCountWindow) {
    // Window expired, reset counter
    info.windowStart = now;
    info.blockCount = 1;
  } else {
    info.blockCount++;
  }

  if (info.blockCount >= updateGroupConfig_.slowPeerBlockCountThreshold) {
    XLOGF(
        INFO,
        "Group {}: Peer {} at bit {} exceeded block frequency threshold "
        "({} blocks in window)",
        groupDescriptor_,
        adjRib->getPeerName(),
        bitPos,
        info.blockCount);
    detachSlowPeer(adjRib, sendWithEoR);
    return;
  }

  // Duration-based detection: schedule timer on AdjRib
  adjRib->scheduleSlowPeerDurationTimer(
      evb_, updateGroupConfig_.slowPeerTimeThreshold, adjRib);
}

/*
 * Mark a peer as unblocked (queue space available).
 * Clears bitmap bit, cancels duration timer.
 */
void AdjRibOutGroup::markPeerUnblocked(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  uint64_t bitPos = adjRib->getGroupBitPosition();

  BitmapUtils::clearBit(adjRibBlockedBitmap_, bitPos);

  // Cancel duration timer if running
  adjRib->cancelSlowPeerDurationTimer();

  auto peerState = adjRib->getPeerState();
  if (peerState == PeerUpdateState::DETACHED_BLOCKED) {
    XLOGF(
        DBG1,
        "Group {}: Peer {} at bit {} State Transition: {} -> {}",
        groupDescriptor_,
        adjRib->getPeerName(),
        bitPos,
        adjRib->getPeerState(),
        PeerUpdateState::DETACHED_RUNNING);
    adjRib->setPeerState(PeerUpdateState::DETACHED_RUNNING);
    adjRib->activateDetachedModeProcessing();
  } else if (peerState == PeerUpdateState::JOINED_BLOCKED) {
    XLOGF(
        DBG1,
        "Group {}: Peer {} at bit {} State Transition: {} -> {}",
        groupDescriptor_,
        adjRib->getPeerName(),
        bitPos,
        adjRib->getPeerState(),
        PeerUpdateState::JOINED_RUNNING);
    adjRib->setPeerState(PeerUpdateState::JOINED_RUNNING);
  }
}

/*
 * Check if any peer in the bitmap is currently blocked.
 */
bool AdjRibOutGroup::hasBlockedPeers() const noexcept {
  return !BitmapUtils::isAllBitsCleared(adjRibBlockedBitmap_);
}

/*
 * Core peer detachment logic shared by detachSlowPeer and policy re-evaluation.
 * detachPeer is not responsible for setting the peer state transition.
 * The caller determines the appropriate target state.
 */
void AdjRibOutGroup::detachPeer(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  uint64_t bit = adjRib->getGroupBitPosition();

  XLOGF(
      INFO,
      "Group {}: Detaching peer {} at bit {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);

  // 1. Copy egress prefix counts while peer is still IN_SYNC
  adjRib->copyEgressPrefixCountsFrom(stats_);

  // 2. Clone packing list for the peer
  clonePackingListForPeer(adjRib);

  // 3. Mark peer as detached (adds to detachedPeers_, clears sync bit)
  markPeerDetached(adjRib);

  // 4. Peer inherits version fields
  adjRib->setLastSeenRibVersion(lastSeenRibVersion_);
  adjRib->setDetachedRibVersion(lastSeenRibVersion_);

  // 5. Clear blocked bitmap (peer is leaving the group's sync set)
  BitmapUtils::clearBit(adjRibBlockedBitmap_, bit);

  // 6. Cancel slow peer duration timer (may be pending if peer was blocked)
  adjRib->cancelSlowPeerDurationTimer();

  // 7. Register detached consumer at group's CL position
  if (changeListTracker_ && changeListConsumer_ && addPathConsumerBitmap_ &&
      nonAddPathConsumerBitmap_) {
    adjRib->registerDetachedConsumer(
        changeListTracker_,
        changeListConsumer_,
        *addPathConsumerBitmap_,
        *nonAddPathConsumerBitmap_);
  } else {
    XLOGF(
        WARN,
        "Group {}: Cannot register detached consumer for peer {} at bit {} - "
        "missing changeListTracker or consumer bitmaps",
        groupDescriptor_,
        adjRib->getPeerName(),
        bit);
  }

  // 8. Set EoR state if the group has pending EoR
  if (egressEoRsPending_) {
    adjRib->setEgressEoRsPending();
  }

  XLOGF(
      INFO,
      "Group {}: Peer {} at bit {} detached successfully "
      "(lastSeenRibVersion={}, detachedRibVersion={})",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit,
      lastSeenRibVersion_,
      lastSeenRibVersion_);
}

/*
 * Detach a slow peer from the group.
 */
void AdjRibOutGroup::detachSlowPeer(
    const std::shared_ptr<AdjRib>& adjRib,
    bool sendWithEoR) noexcept {
  uint64_t bit = adjRib->getGroupBitPosition();
  BgpStats::incrSlowPeerDetectionCount();

  // If slow peer detachment is disabled by config, skip detachment.
  if (!updateGroupConfig_.allowSlowPeerDetach) {
    XLOGF(
        INFO,
        "Group {}: Slow peer detach disabled by config, "
        "skipping detachment for peer {} at bit {}",
        groupDescriptor_,
        adjRib->getPeerName(),
        bit);
    return;
  }

  // If peer is the last synced peer in the group, skip detachment.
  // Detaching the last synced peer would leave no peers receiving group
  // updates.
  if (numInSyncPeers_ <= 1) {
    XLOGF(
        INFO,
        "Group {}: Peer {} at bit {} is the only synced member, skipping detachment",
        groupDescriptor_,
        adjRib->getPeerName(),
        bit);
    return;
  }

  XLOGF(
      INFO,
      "Group {}: Detaching slow peer {} at bit {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);

  detachPeer(adjRib);

  // Slow-peer-specific: set EoR if caller requires it
  if (sendWithEoR) {
    adjRib->setEgressEoRsPending();
  }

  // Slow-peer-specific: transition JOINED_BLOCKED -> DETACHED_BLOCKED
  XLOGF(
      DBG1,
      "Group {}: Peer {} at bit {} State Transition: {} -> {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit,
      adjRib->getPeerState(),
      PeerUpdateState::DETACHED_BLOCKED);
  adjRib->setPeerState(PeerUpdateState::DETACHED_BLOCKED);
}

/*
 * Mark a peer as detached from the group.
 */
void AdjRibOutGroup::markPeerDetached(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  auto bit = adjRib->getGroupBitPosition();
  detachedPeers_.insert(adjRib);
  clearSyncBit(bit);
  XLOGF(
      DBG2,
      "Group {}: Peer {} at bit {} marked as detached",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);
}

/*
 * Clear detached state for a peer (on rejoin).
 */
void AdjRibOutGroup::markPeerInSync(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  auto bit = adjRib->getGroupBitPosition();
  setSyncBit(bit);
  detachedPeers_.erase(adjRib);
  XLOGF(
      DBG2,
      "Group {}: Peer {} at bit {} marked in sync",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);
}

/*
 * Check for detached peers ready to rejoin and accept them back into the group.
 * Called ONLY after PL drain completes (WAITING -> IDLE transition).
 *
 * DFP peers (DETACHED_READY_TO_JOIN + IS_DETACHED_FAST_PEER) can rejoin
 * anytime — their RIB-OUT is guaranteed identical to the group's, so no
 * collapse is needed.
 *
 * DSP peers (DETACHED_READY_TO_JOIN without IS_DETACHED_FAST_PEER) can only
 * rejoin when the group consumer is at the end of the CL, since both must be
 * at the same position. DSP peers go through tryAcceptPeersToGroup for
 * collapse verification.
 */
void AdjRibOutGroup::checkAndAcceptReadyToJoinPeers() noexcept {
  XLOGF(
      DBG1,
      "Group {}: {} {} detached peers to try rejoin",
      groupDescriptor_,
      detachedPeers_.empty() ? "Skipping" : "Checking",
      detachedPeers_.size());
  bool groupAtEndOfCL = changeListConsumer_ && changeListConsumer_->isReady();

  std::vector<std::shared_ptr<AdjRib>> dfpPeers;
  std::vector<std::shared_ptr<AdjRib>> dspCandidates;

  for (const auto& adjRib : detachedPeers_) {
    if (adjRib->getPeerState() != PeerUpdateState::DETACHED_READY_TO_JOIN) {
      continue;
    }

    if (FOLLY_UNLIKELY(adjRib->testOnlyDeferDrjAcceptance)) {
      continue;
    }

    if (adjRib->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER)) {
      dfpPeers.push_back(adjRib);
    } else if (groupAtEndOfCL && adjRib->isReadyToRejoinGroup()) {
      dspCandidates.push_back(adjRib);
    } else {
      /*
       * Peers that were in DETACHED_READY_TO_JOIN state must resume consuming
       * changelist since they cannot be rejoined currently, due to two possible
       * reasons:
       *   (1) The group is no longer changelist consumer READY
       *   (2) The candidate DSP peer is no longer changelist consumer READY
       * The peer must try to rejoin again after consuming more changes.
       */
      XLOGF(
          DBG1,
          "Group {}: Peer {} at bit {} State Transition: {} (DSP) -> {}",
          groupDescriptor_,
          adjRib->getPeerName(),
          adjRib->getGroupBitPosition(),
          adjRib->getPeerState(),
          PeerUpdateState::DETACHED_RUNNING);
      adjRib->setPeerState(PeerUpdateState::DETACHED_RUNNING);
      adjRib->reschedulePackingTimers();
    }
  }

  // Accept DFP peers directly (no collapse needed, RIB-OUT identical to group)
  for (const auto& peer : dfpPeers) {
    auto bit = peer->getGroupBitPosition();
    peer->clearAdjRibFlag(AdjRib::IS_DETACHED_FAST_PEER);
    peer->deactivateDetachedModeProcessing();
    // Reset block info on rejoin rather than on detach activation, because
    // block info is used for frequency-based slow peer detection and should
    // only be reset when the peer rejoins the group with a fresh start.
    peer->resetPeerBlockInfo();
    XLOGF(
        DBG1,
        "Group {}: Peer {} at bit {} State Transition: {} (DFP) -> {}",
        groupDescriptor_,
        peer->getPeerName(),
        bit,
        peer->getPeerState(),
        PeerUpdateState::JOINED_RUNNING);
    peer->setPeerState(PeerUpdateState::JOINED_RUNNING);
    XLOGF(
        INFO,
        "Group {}: Peer {} at bit {} successfully rejoined group",
        groupDescriptor_,
        peer->getPeerName(),
        bit);
    markPeerInSync(peer);
  }

  // Accept DSP peers via collapse verification
  if (!dspCandidates.empty()) {
    auto acceptedPeers = tryAcceptPeersToGroup(dspCandidates);
    for (const auto& peer : acceptedPeers) {
      markPeerInSync(peer);
    }
  }
}

bool AdjRibOutGroup::isGroupConsumerReady() const noexcept {
  return changeListConsumer_ && changeListConsumer_->isReady();
}

/*
 * Called by a DSP peer to proactively trigger its own rejoin.
 * Only accepts if the group consumer is at end of CL.
 */
void AdjRibOutGroup::checkAndAcceptDSPPeer(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  if (FOLLY_UNLIKELY(adjRib->testOnlyDeferDrjAcceptance)) {
    return;
  }
  auto bit = adjRib->getGroupBitPosition();
  XLOGF(
      INFO,
      "Group {}: Attempting to rejoin group from peer {} at bit {}",
      groupDescriptor_,
      adjRib->getPeerName(),
      bit);
  if (tryAcceptPeerToGroup(adjRib)) {
    markPeerInSync(adjRib);
  }
}

bool AdjRibOutGroup::tryAcceptPeerToGroup(
    const std::shared_ptr<AdjRib>& candidatePeer) noexcept {
  auto accepted = tryAcceptPeersToGroup({candidatePeer});
  return !accepted.empty();
}

void AdjRibOutGroup::testOnlySetDeferDrjAcceptance(
    const folly::IPAddress& peerAddr,
    bool defer) noexcept {
  for (const auto& [_, adjRib] : bitToAdjRibs_) {
    if (adjRib->getPeerAddress() == peerAddr) {
      adjRib->testOnlyDeferDrjAcceptance = defer;
      break;
    }
  }
  if (!defer) {
    checkAndAcceptReadyToJoinPeers();
  }
}

/*
 * Try to accept DSP candidate peers back into the group in batch.
 * Splits candidates into addPath and non-addPath, collapses RIB-OUT
 * entries in one tree walk per type. Matching peers are accepted
 * (JOINED_RUNNING); mismatched peers are set back to DETACHED_RUNNING.
 */
std::vector<std::shared_ptr<AdjRib>> AdjRibOutGroup::tryAcceptPeersToGroup(
    const std::vector<std::shared_ptr<AdjRib>>& candidatePeers) noexcept {
  // Split candidates into:
  //   pathPeers: sendAddPath peers whose entries live in AdjRibPathTree
  //   litePeers: non-addPath peers whose entries live in AdjRibLiteTree
  std::vector<std::shared_ptr<AdjRib>> pathPeers;
  std::vector<std::shared_ptr<AdjRib>> litePeers;
  for (const auto& peer : candidatePeers) {
    XLOGF(
        INFO,
        "Group {}: Trying peer {} rejoin at bit {}",
        groupDescriptor_,
        peer->getPeerName(),
        peer->getGroupBitPosition());
    if (peer->sendAddPath()) {
      pathPeers.push_back(peer);
    } else {
      litePeers.push_back(peer);
    }
  }

  // Collapse per-peer RIB-OUT entries back into the group.
  // DSP entries are compared and corrections queued in the packing list.
  // Peers with discrepancies have RIB_OUT_DISCREPANCY flag set.
  auto groupOwnerKey = getGroupOwnerKey();
  if (!pathPeers.empty()) {
    collapsePathEntries(groupOwnerKey, pathPeers);
  }
  if (!litePeers.empty()) {
    collapseLiteEntries(groupOwnerKey, litePeers);
  }

  // Finalize each peer
  std::vector<std::shared_ptr<AdjRib>> acceptedPeers;
  for (const auto& peer : candidatePeers) {
    auto bit = peer->getGroupBitPosition();

    if (peer->isAdjRibFlagSet(AdjRib::RIB_OUT_DISCREPANCY)) {
      XLOGF(
          ERR,
          "Group {}: Peer {} at bit {} has entry discrepancies, "
          "not rejoining group",
          groupDescriptor_,
          peer->getPeerName(),
          bit);
      // TODO: bump stats/counters for rejoin discrepancy
      XLOGF(
          DBG1,
          "Group {}: Peer {} at bit {} State Transition: {} (DSP) -> {}",
          groupDescriptor_,
          peer->getPeerName(),
          bit,
          peer->getPeerState(),
          PeerUpdateState::DETACHED_RUNNING);
      peer->setPeerState(PeerUpdateState::DETACHED_RUNNING);
      /*
       * Update detachedRibVersion to the group's current version so that
       * the next collapse attempt only flags entries newer than this point.
       * Without this, the same group-only entries keep being detected as
       * discrepancies on every rejoin attempt (infinite loop).
       */
      peer->setDetachedRibVersion(lastSeenRibVersion_);
      peer->reschedulePackingTimers();
      peer->clearAdjRibFlag(AdjRib::RIB_OUT_DISCREPANCY);
      continue;
    }

    peer->deactivateDetachedModeProcessing();
    peer->clearAdjRibFlag(AdjRib::WAS_DETACHED_INIT_DUMP_PEER);
    // Reset block info on rejoin rather than on detach activation, because
    // block info is used for frequency-based slow peer detection and should
    // only be reset when the peer rejoins the group with a fresh start.
    peer->resetPeerBlockInfo();
    XLOGF(
        DBG1,
        "Group {}: Peer {} at bit {} State Transition: {} (DSP) -> {}",
        groupDescriptor_,
        peer->getPeerName(),
        bit,
        peer->getPeerState(),
        PeerUpdateState::JOINED_RUNNING);
    peer->setPeerState(PeerUpdateState::JOINED_RUNNING);
    XLOGF(
        INFO,
        "Group {}: Peer {} at bit {} successfully rejoined group",
        groupDescriptor_,
        peer->getPeerName(),
        bit);
    acceptedPeers.push_back(peer);
  }
  return acceptedPeers;
}

/*
 * Deep-copy the group's current packing list to the peer on detachment.
 */
void AdjRibOutGroup::clonePackingListForPeer(
    const std::shared_ptr<AdjRib>& adjRib) noexcept {
  // Copy the group's packing list to the peer
  adjRib->setDetachedPackingList(attrToPrefixMap_);

  XLOGF(
      DBG2,
      "Group {}: Cloned packing list ({} attr entries) for peer {} at bit {}",
      groupDescriptor_,
      attrToPrefixMap_.size(),
      adjRib->getPeerName(),
      adjRib->getGroupBitPosition());
}

bool AdjRibOutGroup::shouldClonePathForPeer(
    const AdjRibPathTree::Iterator& radixNodeItr,
    const uint32_t pathId,
    const std::shared_ptr<AdjRib>& peer,
    const uint64_t groupEntryRibVersion) noexcept {
  auto peerOwnerKey = peer->getPeerOwnerKey();

  // Case 1: peer already has its own entry for this prefix — no clone needed
  if (getAdjRibEntryFromPathNodeItr(radixNodeItr, peerOwnerKey, pathId)) {
    return false;
  }

  // Case 2: entry was announced/re-announced after peer detached — no clone
  if (isEntryNotShared(peer->getDetachedRibVersion(), groupEntryRibVersion)) {
    return false;
  }

  // Case 3: peer was sharing this entry when detached — must clone
  return true;
}

bool AdjRibOutGroup::shouldCloneLiteForPeer(
    const AdjRibLiteTree::Iterator& radixNodeItr,
    const std::shared_ptr<AdjRib>& peer,
    const uint64_t groupEntryRibVersion) noexcept {
  auto peerOwnerKey = peer->getPeerOwnerKey();

  // Case 1: peer already has its own entry for this prefix — no clone needed
  if (getAdjRibEntryFromLiteNodeItr(radixNodeItr, peerOwnerKey)) {
    return false;
  }

  // Case 2: entry was announced/re-announced after peer detached — no clone
  if (isEntryNotShared(peer->getDetachedRibVersion(), groupEntryRibVersion)) {
    return false;
  }

  // Case 3: peer was sharing this entry when detached — must clone
  return true;
}

AdjRibEntry* AdjRibOutGroup::copyEntryForPeer(
    const folly::CIDRNetwork& prefix,
    uint32_t pathId,
    const std::shared_ptr<AdjRib>& peer,
    const AdjRibOutOwnerKey& effectiveOwnerKey,
    const AdjRibEntry* entryToCopy) noexcept {
  auto peerEntry = addRibEntry(prefix, effectiveOwnerKey, pathId);

  peerEntry->flags_ = entryToCopy->flags_;
  peerEntry->setPreOut(entryToCopy->getPreOut());
  peerEntry->setPostAttr(entryToCopy->getPostAttr());
  if (entryToCopy->getPostOutPolicy()) {
    peerEntry->setPostOutPolicy(*entryToCopy->getPostOutPolicy());
  }
  peerEntry->setRibVersion(entryToCopy->getRibVersion());

  XLOGF(
      DBG3,
      "Group {}: Cloned entry for {} to peer {} at bit {}",
      groupDescriptor_,
      folly::IPAddress::networkToString(prefix),
      peer->getPeerName(),
      peer->getGroupBitPosition());

  return peerEntry;
}

void AdjRibOutGroup::lazyClonePathForDetachedPeers(
    const folly::CIDRNetwork& prefix,
    uint32_t pathId,
    const AdjRibPathTree::Iterator& radixNodeItr,
    const AdjRibEntry* groupEntry) noexcept {
  for (const auto& adjRib : detachedPeers_) {
    if (shouldClonePathForPeer(
            radixNodeItr, pathId, adjRib, groupEntry->getRibVersion())) {
      copyEntryForPeer(
          prefix, pathId, adjRib, adjRib->getPeerOwnerKey(), groupEntry);
    }
  }
}

void AdjRibOutGroup::lazyCloneLiteForDetachedPeers(
    const folly::CIDRNetwork& prefix,
    uint32_t pathId,
    const AdjRibLiteTree::Iterator& radixNodeItr,
    const AdjRibEntry* groupEntry) noexcept {
  for (const auto& adjRib : detachedPeers_) {
    if (shouldCloneLiteForPeer(
            radixNodeItr, adjRib, groupEntry->getRibVersion())) {
      copyEntryForPeer(
          prefix, pathId, adjRib, adjRib->getPeerOwnerKey(), groupEntry);
    }
  }
}

bool AdjRibOutGroup::collapsePathEntry(
    PathOwnerMap& ownerMap,
    const AdjRibOutOwnerKey& groupOwnerKey,
    const folly::CIDRNetwork& prefix,
    const std::shared_ptr<AdjRib>& peer) {
  auto peerOwnerKey = peer->getPeerOwnerKey();
  auto groupIt = ownerMap.find(groupOwnerKey);
  auto peerIt = ownerMap.find(peerOwnerKey);

  if (peerIt != ownerMap.end() && groupIt == ownerMap.end()) {
    // Peer-only entries: collapse and queue withdrawal for each pathId
    for (const auto& [pathId, peerEntry] : peerIt->second) {
      auto prefixPathId = std::make_pair(prefix, pathId);
      XLOGF_EVERY_MS(
          ERR,
          10000,
          "Group {}: collapse path withdrawal for peer {} prefix {} pathId {}",
          groupDescriptor_,
          peer->getPeerName(),
          folly::IPAddress::networkToString(prefix),
          pathId);
      peer->tryUpdateAttrToPrefixMap(
          prefixPathId, peerEntry->getPostAttr(), nullptr);
      if (peerEntry->getPostAttr()) {
        peer->decrementPostOutPrefixCount(prefix.first.isV4());
      }
      peer->decrementPreOutPrefixCount(prefix.first.isV4());
    }
    ownerMap.erase(peerOwnerKey);
    return true;
  } else if (peerIt == ownerMap.end() && groupIt != ownerMap.end()) {
    /*
     * Group-only entries: queue announcements for post-detach pathIds.
     * For init dump peers (WAS_DETACHED_INIT_DUMP_PEER), skip the rib version
     * check since they have no meaningful detachedRibVersion — they need all
     * entries.
     */
    bool isDetachedInitDump =
        peer->isAdjRibFlagSet(AdjRib::WAS_DETACHED_INIT_DUMP_PEER);
    bool hasDiscrepancy = isDetachedInitDump;
    for (const auto& [pathId, groupEntry] : groupIt->second) {
      if (isDetachedInitDump ||
          groupEntry->getRibVersion() > peer->getDetachedRibVersion()) {
        /* Peer never saw this entry, queue announcement (discrepancy found). */
        auto prefixPathId = std::make_pair(prefix, pathId);
        if (isDetachedInitDump) {
          XLOGF_EVERY_MS(
              ERR,
              10000,
              "Group {}: collapse path announcement for peer {} prefix {} "
              "pathId {} (init dump peer, no meaningful detachedRibVersion)",
              groupDescriptor_,
              peer->getPeerName(),
              folly::IPAddress::networkToString(prefix),
              pathId);
        } else {
          XLOGF_EVERY_MS(
              ERR,
              10000,
              "Group {}: collapse path announcement for peer {} prefix {} "
              "pathId {} (ribVersion {} > detachedVersion {})",
              groupDescriptor_,
              peer->getPeerName(),
              folly::IPAddress::networkToString(prefix),
              pathId,
              groupEntry->getRibVersion(),
              peer->getDetachedRibVersion());
        }
        peer->tryUpdateAttrToPrefixMap(
            prefixPathId, nullptr, groupEntry->getPostAttr());
        if (groupEntry->getPostAttr()) {
          peer->incrementPostOutPrefixCount(prefix.first.isV4());
        }
        peer->incrementPreOutPrefixCount(prefix.first.isV4());
        hasDiscrepancy = true;
      }
    }
    /*
     * Returns true if there is a discrepancy, indicating the RIB-OUT between
     * group and peer does NOT match for this case, where peer entry is missing
     * and group entry exists.
     *
     * Some special notes:
     * For DETACHED_INIT_DUMP peers whose RIB dump entries are being collapsed,
     * these peers do not have 'implicit' entries because they are rejoining the
     * group from a full RIB walk and MUST have their own entries. Hence, in
     * this block of if-else where peer entry is missing,
     * hasDiscrepancy is ALWAYS true (discrepancy found).
     *
     * For non DETACHED_INIT_DUMP peers that naturally detached after running
     * with the group, they can have implicit entries (i.e. sharing an entry
     * with the group) if the group entry ribVersion <= peer detachedRibVersion.
     */
    return hasDiscrepancy;
  } else if (peerIt != ownerMap.end() && groupIt != ownerMap.end()) {
    // Both have entries: collapse peer, queue corrections for mismatches
    bool hasDiscrepancy = false;
    for (const auto& [pathId, peerEntry] : peerIt->second) {
      auto groupPidIt = groupIt->second.find(pathId);
      if (groupPidIt == groupIt->second.end()) {
        // Peer pathId not in group: queue withdrawal
        auto prefixPathId = std::make_pair(prefix, pathId);
        XLOGF_EVERY_MS(
            ERR,
            10000,
            "Group {}: collapse path withdrawal for peer {} prefix {} pathId {}",
            groupDescriptor_,
            peer->getPeerName(),
            folly::IPAddress::networkToString(prefix),
            pathId);
        peer->tryUpdateAttrToPrefixMap(
            prefixPathId, peerEntry->getPostAttr(), nullptr);
        if (peerEntry->getPostAttr()) {
          peer->decrementPostOutPrefixCount(prefix.first.isV4());
        }
        peer->decrementPreOutPrefixCount(prefix.first.isV4());
        hasDiscrepancy = true;
      } else if (!peerEntry->hasMatchingPostPolicyAttrs(
                     groupPidIt->second.get())) {
        // Attrs mismatch: queue re-advertisement with group's attrs
        auto prefixPathId = std::make_pair(prefix, pathId);
        XLOGF_EVERY_MS(
            ERR,
            10000,
            "Group {}: collapse path re-advertise for peer {} prefix {} pathId {}",
            groupDescriptor_,
            peer->getPeerName(),
            folly::IPAddress::networkToString(prefix),
            pathId);
        peer->tryUpdateAttrToPrefixMap(
            prefixPathId, nullptr, groupPidIt->second->getPostAttr());
        hasDiscrepancy = true;
      }
    }
    // Group pathIds that peer doesn't have: queue announcements
    for (const auto& [pathId, groupEntry] : groupIt->second) {
      if (peerIt->second.find(pathId) == peerIt->second.end()) {
        auto prefixPathId = std::make_pair(prefix, pathId);
        XLOGF_EVERY_MS(
            ERR,
            10000,
            "Group {}: collapse path announcement for peer {} prefix {} pathId {}",
            groupDescriptor_,
            peer->getPeerName(),
            folly::IPAddress::networkToString(prefix),
            pathId);
        peer->tryUpdateAttrToPrefixMap(
            prefixPathId, nullptr, groupEntry->getPostAttr());
        if (groupEntry->getPostAttr()) {
          peer->incrementPostOutPrefixCount(prefix.first.isV4());
        }
        peer->incrementPreOutPrefixCount(prefix.first.isV4());
        hasDiscrepancy = true;
      }
    }
    ownerMap.erase(peerOwnerKey);
    return hasDiscrepancy;
  }
  /*
   * Neither peer nor group has entry at this node, which is equivalent
   * to having the same RIB-OUT — no discrepancy. This is reachable
   * because we iterate the entire radix tree for a batch of peers.
   * For example, collapsePathEntries({peer1, peer2}) walks every prefix
   * and calls collapsePathEntry for each peer. If peer1 has an entry
   * but peer2 and the group don't, peer1 gets RIB_OUT_DISCREPANCY
   * while peer2 falls through here (matching the group's absent state).
   */
  return false;
}

void AdjRibOutGroup::collapsePathEntries(
    const AdjRibOutOwnerKey& groupOwnerKey,
    const std::vector<std::shared_ptr<AdjRib>>& peers) {
  std::vector<AdjRibPathTree::Iterator> emptyNodes;
  folly::F14FastMap<uint64_t, size_t> failedPrefixCount;

  for (auto itr = PathTree_.begin(); itr != PathTree_.end(); ++itr) {
    auto prefix = folly::CIDRNetwork{itr.ipAddress(), itr.masklen()};
    for (const auto& peer : peers) {
      if (collapsePathEntry(itr->value(), groupOwnerKey, prefix, peer)) {
        peer->setAdjRibFlag(AdjRib::RIB_OUT_DISCREPANCY);
        incrTotalDiscrepancies();
        ++failedPrefixCount[peer->getGroupBitPosition()];
      }
    }
    if (itr->value().empty()) {
      emptyNodes.push_back(itr);
    }
  }

  for (const auto& [bit, count] : failedPrefixCount) {
    auto it = bitToAdjRibs_.find(bit);
    if (it == bitToAdjRibs_.end()) {
      continue;
    }
    XLOGF(
        ERR,
        "Group {}: collapse completed for peer {} (bit {}) "
        "with {}/{} prefixes in discrepancy",
        groupDescriptor_,
        it->second->getPeerName(),
        bit,
        count,
        PathTree_.size());
  }

  for (auto& itr : emptyNodes) {
    PathTree_.erase(itr);
  }
}

bool AdjRibOutGroup::collapseLiteEntry(
    LiteOwnerMap& ownerMap,
    const AdjRibOutOwnerKey& groupOwnerKey,
    const folly::CIDRNetwork& prefix,
    const std::shared_ptr<AdjRib>& peer) {
  auto peerOwnerKey = peer->getPeerOwnerKey();
  auto groupIt = ownerMap.find(groupOwnerKey);
  auto peerIt = ownerMap.find(peerOwnerKey);

  if (peerIt != ownerMap.end() && groupIt == ownerMap.end()) {
    // Peer-only entry: collapse and queue withdrawal
    XLOGF_EVERY_MS(
        ERR,
        10000,
        "Group {}: collapse discrepancy for peer {} prefix {}: "
        "peer-only entry (no group entry), queuing withdrawal",
        groupDescriptor_,
        peer->getPeerName(),
        folly::IPAddress::networkToString(prefix));
    auto prefixPathId = std::make_pair(prefix, peerIt->second->getPathId());
    peer->tryUpdateAttrToPrefixMap(
        prefixPathId, peerIt->second->getPostAttr(), nullptr);
    if (peerIt->second->getPostAttr()) {
      peer->decrementPostOutPrefixCount(prefix.first.isV4());
    }
    peer->decrementPreOutPrefixCount(prefix.first.isV4());
    ownerMap.erase(peerOwnerKey);
    return true;
  } else if (peerIt == ownerMap.end() && groupIt != ownerMap.end()) {
    /*
     * Group-only entry: for init dump peers (WAS_DETACHED_INIT_DUMP_PEER),
     * skip the rib version check since they have no meaningful
     * detachedRibVersion — they need all entries. For DSP peers, only
     * post-detach entries (ribVersion > detachedRibVersion) are discrepancies.
     */
    bool isDetachedInitDump =
        peer->isAdjRibFlagSet(AdjRib::WAS_DETACHED_INIT_DUMP_PEER);
    if (isDetachedInitDump ||
        groupIt->second->getRibVersion() > peer->getDetachedRibVersion()) {
      /* Peer never saw this entry, queue announcement (discrepancy found). */
      auto prefixPathId = std::make_pair(prefix, kPlaceholderPathID);
      if (isDetachedInitDump) {
        XLOGF_EVERY_MS(
            ERR,
            10000,
            "Group {}: collapse discrepancy for peer {} prefix {}: "
            "group-only entry (init dump peer, no meaningful detachedRibVersion), "
            "queuing announcement",
            groupDescriptor_,
            peer->getPeerName(),
            folly::IPAddress::networkToString(prefix));
      } else {
        XLOGF_EVERY_MS(
            ERR,
            10000,
            "Group {}: collapse discrepancy for peer {} prefix {}: "
            "group-only entry (ribVersion {} > detachedVersion {}), "
            "queuing announcement",
            groupDescriptor_,
            peer->getPeerName(),
            folly::IPAddress::networkToString(prefix),
            groupIt->second->getRibVersion(),
            peer->getDetachedRibVersion());
      }
      peer->tryUpdateAttrToPrefixMap(
          prefixPathId, nullptr, groupIt->second->getPostAttr());
      if (groupIt->second->getPostAttr()) {
        peer->incrementPostOutPrefixCount(prefix.first.isV4());
      }
      peer->incrementPreOutPrefixCount(prefix.first.isV4());
      return true; // discrepancy: correction queued
    }
    /*
     * Shared pre-detach entry: peer already has group's view, no correction
     * needed. Init dump peers will not reach this line — they always take
     * the branch above.
     */
    return false; // no discrepancy
  } else if (peerIt != ownerMap.end() && groupIt != ownerMap.end()) {
    if (peerIt->second->hasMatchingPostPolicyAttrs(groupIt->second.get())) {
      // Match: collapse peer entry, no packing list correction needed
      ownerMap.erase(peerOwnerKey);
      return false;
    }
    // Mismatch: collapse peer entry, queue re-advertisement with group's attrs
    auto prefixPathId = std::make_pair(prefix, kPlaceholderPathID);
    XLOGF_EVERY_MS(
        ERR,
        10000,
        "Group {}: collapse discrepancy for peer {} prefix {}: "
        "peer/group attrs mismatch, queuing re-advertisement",
        groupDescriptor_,
        peer->getPeerName(),
        folly::IPAddress::networkToString(prefix));
    peer->tryUpdateAttrToPrefixMap(
        prefixPathId, nullptr, groupIt->second->getPostAttr());
    ownerMap.erase(peerOwnerKey);
    return true;
  }
  /*
   * Neither peer nor group has entry at this node, which is equivalent
   * to having the same RIB-OUT — no discrepancy. This is reachable
   * because we iterate the entire radix tree for a batch of peers.
   * For example, collapseLiteEntries({peer1, peer2}) walks every prefix
   * and calls collapseLiteEntry for each peer. If peer1 has an entry
   * but peer2 and the group don't, peer1 gets RIB_OUT_DISCREPANCY
   * while peer2 falls through here (matching the group's absent state).
   */
  return false;
}

void AdjRibOutGroup::collapseLiteEntries(
    const AdjRibOutOwnerKey& groupOwnerKey,
    const std::vector<std::shared_ptr<AdjRib>>& peers) {
  std::vector<AdjRibLiteTree::Iterator> emptyNodes;
  folly::F14FastMap<uint64_t, size_t> failedPrefixCount;

  for (auto itr = LiteTree_.begin(); itr != LiteTree_.end(); ++itr) {
    auto prefix = folly::CIDRNetwork{itr.ipAddress(), itr.masklen()};
    for (const auto& peer : peers) {
      if (collapseLiteEntry(itr->value(), groupOwnerKey, prefix, peer)) {
        peer->setAdjRibFlag(AdjRib::RIB_OUT_DISCREPANCY);
        incrTotalDiscrepancies();
        ++failedPrefixCount[peer->getGroupBitPosition()];
      }
    }
    if (itr->value().empty()) {
      emptyNodes.push_back(itr);
    }
  }

  for (const auto& [bit, count] : failedPrefixCount) {
    auto it = bitToAdjRibs_.find(bit);
    if (it == bitToAdjRibs_.end()) {
      continue;
    }
    XLOGF(
        ERR,
        "Group {}: collapse completed for peer {} (bit {}) "
        "with {}/{} prefixes in discrepancy",
        groupDescriptor_,
        it->second->getPeerName(),
        bit,
        count,
        LiteTree_.size());
  }

  for (auto& itr : emptyNodes) {
    LiteTree_.erase(itr);
  }
}

} // namespace facebook::bgp
