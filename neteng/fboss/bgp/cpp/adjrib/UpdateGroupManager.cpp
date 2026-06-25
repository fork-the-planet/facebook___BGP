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

#include <fmt/format.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibGroup.h"
#include "neteng/fboss/bgp/cpp/adjrib/UpdateGroupManager.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"

namespace facebook::bgp {

std::shared_ptr<AdjRibOutGroup> UpdateGroupManager::findOrCreateGroup(
    const UpdateGroupKey& key) {
  auto it = updateGroups_.find(key);
  if (it != updateGroups_.end()) {
    return it->second;
  }

  auto groupId = nextGroupId_++;
  auto groupName = UpdateGroupKey::toString(key);
  auto group = std::make_shared<AdjRibOutGroup>(
      evb_,
      groupName,
      groupId,
      true /* enableUpdateGroup */,
      key,
      shadowRibEntries_,
      policyManager_ /* policyManager */,
      updateGroupConfig_);
  updateGroups_[key] = group;

  /*
   * Auto-trigger initial dump if BGP is already initialized.
   * This handles the case where new groups are created after initial BGP init.
   */
  if (isRibInitDone_ && isRibInitDone_()) {
    if (group->getMemberCount() > 0) {
      group->scheduleInitialDump();
      XLOGF(
          INFO,
          "BGP already initialized - triggering initial dump for new group: {}",
          group->getGroupDescriptor());
    }
  }

  BgpStats::incrNumUpdateGroups();

  XLOGF(INFO, "Update group: {} is created", group->getGroupDescriptor());

  // TODO: register with changeListTracker to not lose subsequent updates
  return group;
}

std::shared_ptr<AdjRibOutGroup> UpdateGroupManager::getGroup(
    const UpdateGroupKey& key) const {
  auto it = updateGroups_.find(key);
  if (it != updateGroups_.end()) {
    return it->second;
  }
  return nullptr;
}

folly::coro::Task<void> UpdateGroupManager::maybeDestroyUpdateGroup(
    const UpdateGroupKey& key) {
  auto it = updateGroups_.find(key);
  if (it == updateGroups_.end()) {
    co_return;
  }

  auto& group = it->second;
  if (group->getMemberCount() != 0) {
    co_return;
  }

  group->resetChangeListConsumeTimer();
  group->deactivateChangeListConsumer();
  group->resetChangeListConsumer();
  group->clearPackingList();

  auto g = std::move(it->second);
  updateGroups_.erase(it);
  BgpStats::decrNumUpdateGroups();

  /*
   * Cancel any pending coroutines in asyncScope_ and cooperatively
   * wait for them to complete. This must happen before the group is
   * destroyed to avoid blocking the EventBase thread in the destructor.
   */
  co_await g->drainAsyncScope();
}

void UpdateGroupManager::rekeyGroup(
    const std::shared_ptr<AdjRibOutGroup>& group) {
  auto oldKey = group->getGroupKey();

  // Rebuild UpdateGroupKey on all peers in the group
  auto& bitToAdjRibs = group->getBitToAdjRibs();
  if (bitToAdjRibs.empty()) {
    XLOGF(
        ERR,
        "Group {}: unexpected empty group while updating group key",
        group->getGroupDescriptor());
    return;
  }

  for (const auto& [_, adjRib] : bitToAdjRibs) {
    adjRib->buildAndSetUpdateGroupKey();
  }

  auto& newKey = bitToAdjRibs.begin()->second->getUpdateGroupKey();

  if (oldKey == newKey) {
    XLOGF(
        DBG1,
        "Group {}: rekeyGroup no-op, key unchanged ({})",
        group->getGroupDescriptor(),
        UpdateGroupKey::toString(oldKey));
    return;
  }

  group->setGroupKey(newKey);

  auto [it, inserted] = updateGroups_.try_emplace(newKey, group);
  if (!inserted) {
    XLOGF(
        ERR,
        "Group {}: rekeyGroup found existing group {} at new key {}, overwriting",
        group->getGroupDescriptor(),
        it->second->getGroupDescriptor(),
        UpdateGroupKey::toString(newKey));
    it->second = group;
  }

  updateGroups_.erase(oldKey);

  XLOGF(
      INFO,
      "Group {}: Group key changed from {} to {}",
      group->getGroupDescriptor(),
      UpdateGroupKey::toString(oldKey),
      UpdateGroupKey::toString(newKey));
}

void UpdateGroupManager::setUpdateGroupState(
    const UpdateGroupKey& key,
    UpdateGroupState state) {
  auto group = getGroup(key);
  if (!group) {
    return;
  }

  // Suppress unused parameter warning until implementation
  (void)state;

  /*
   * TODO: the group state set will be implemented later
   */
}

void UpdateGroupManager::triggerInitialDumpsForUninitializedGroups() noexcept {
  /*
   * Walk all update groups and trigger initial dump for those in UNINITIALIZED
   * state. Called when BGP initialization completes
   * (markRibInitialAnnouncementDone).
   */
  for (const auto& [key, group] : updateGroups_) {
    if (group->getState() == UpdateGroupState::UNINITIALIZED) {
      // Only trigger if group has members
      if (group->getMemberCount() > 0) {
        group->scheduleInitialDump();
        XLOGF(
            INFO,
            "Triggered initial dump for update group: {}",
            group->getGroupDescriptor());
      } else {
        XLOGF(
            WARN,
            "Skipping initial dump for empty update group: {}",
            group->getGroupDescriptor());
      }
    }
  }
}

void UpdateGroupManager::testOnlySetDeferDrjAcceptance(
    const folly::IPAddress& peerAddr,
    bool defer) noexcept {
  for (auto& [_, group] : updateGroups_) {
    group->testOnlySetDeferDrjAcceptance(peerAddr, defer);
  }
}

} // namespace facebook::bgp
