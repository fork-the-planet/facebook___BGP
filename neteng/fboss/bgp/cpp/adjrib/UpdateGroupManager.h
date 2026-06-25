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

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/io/async/EventBase.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStructs.h"
#include "neteng/fboss/bgp/cpp/adjrib/ShadowRibTypes.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"

namespace facebook::bgp {

class AdjRibOutGroup;
class PolicyManager;

/**
 * UpdateGroupManager manages the lifecycle of all update-groups.
 * It provides group lookup, creation, and deletion functionality.
 *
 * Peers with identical UpdateGroupKey are assigned to the same update group
 * to optimize memory and CPU by sharing packing lists and update generation.
 */
class UpdateGroupManager {
 public:
  explicit UpdateGroupManager(
      folly::EventBase& evb,
      const UpdateGroupConfig& updateGroupConfig,
      const ShadowRibEntriesMap* shadowRibEntries = nullptr,
      std::shared_ptr<PolicyManager> policyManager = nullptr,
      std::function<bool()> isRibInitDone = nullptr)
      : evb_(evb),
        updateGroupConfig_(updateGroupConfig),
        shadowRibEntries_(shadowRibEntries),
        policyManager_(std::move(policyManager)),
        isRibInitDone_(std::move(isRibInitDone)) {}
  ~UpdateGroupManager() = default;

  /*
   * @brief: find or create an update group for the given key.
   * @param: update group key
   * @return: shared_ptr of the existing group, or the newly created one.
   */
  std::shared_ptr<AdjRibOutGroup> findOrCreateGroup(const UpdateGroupKey& key);

  /*
   * @brief: destroy update group if it has no members.
   * Cooperatively drains asyncScope_ before destruction to avoid
   * blocking the EventBase thread.
   * @param: update group key
   * @note: this is called during peer termination to cleanup empty groups.
   * @return: Task<void> to be co_awaited by the caller.
   */
  folly::coro::Task<void> maybeDestroyUpdateGroup(const UpdateGroupKey& key);

  /*
   * @brief: set the state of an update group using ENUM.
   * @param: update group key
   * @param: ENUM state
   * @return: none
   */
  void setUpdateGroupState(const UpdateGroupKey& key, UpdateGroupState state);

  /*
   * Get the number of update groups currently managed.
   */
  uint64_t getGroupCount() const {
    return updateGroups_.size();
  }

  /*
   * Check if a group exists for the given key.
   */
  bool hasGroup(const UpdateGroupKey& key) const {
    return updateGroups_.contains(key);
  }

  /*
   * Get an existing group for the given key.
   * Returns nullptr if no group exists.
   */
  std::shared_ptr<AdjRibOutGroup> getGroup(const UpdateGroupKey& key) const;

  /*
   * Get a read-only reference to all update groups.
   * Used by PeerManager to build TUpdateGroupInfo for CLI/thrift.
   */
  const folly::F14NodeMap<
      UpdateGroupKey,
      std::shared_ptr<AdjRibOutGroup>,
      UpdateGroupKeyHash>&
  getAllGroups() const noexcept {
    return updateGroups_;
  }

  /*
   * Re-key a group after egress policy re-evaluation.
   * Calls buildAndSetUpdateGroupKey on all peers in the group,
   * then moves the group's entry in the map to the new key.
   */
  void rekeyGroup(const std::shared_ptr<AdjRibOutGroup>& group);

  /*
   * Trigger initial dumps for all update groups in UNINITIALIZED state.
   * Called when BGP initialization completes.
   */
  void triggerInitialDumpsForUninitializedGroups() noexcept;

  /*
   * Test-only: defer DRJ acceptance for a specific peer across all
   * update groups. The peer belongs to exactly one group.
   */
  void testOnlySetDeferDrjAcceptance(
      const folly::IPAddress& peerAddr,
      bool defer) noexcept;

 private:
  /*
   * EventBase reference for scheduling async operations.
   * Passed to AdjRibOutGroup during construction.
   */
  folly::EventBase& evb_;

  /*
   * Map from UpdateGroupKey to AdjRibOutGroup.
   * Each unique key corresponds to one update group.
   */
  folly::F14NodeMap<
      UpdateGroupKey,
      std::shared_ptr<AdjRibOutGroup>,
      UpdateGroupKeyHash>
      updateGroups_;

  /*
   * A monotonically increasing value of groupId to uniquely identify a group.
   */
  uint64_t nextGroupId_{0};

  /*
   * Update group configuration: serialization mode, slow peer detection
   * thresholds, and detachment behavior.
   * Passed from PeerManager and applied to all update groups on creation.
   */
  UpdateGroupConfig updateGroupConfig_;

  /*
   * Reference to PeerManager's shadow RIB entries.
   * Passed to update groups for initial dumps and change tracking.
   */
  const ShadowRibEntriesMap* shadowRibEntries_{nullptr};

  /*
   * Shared_ptr to the policyManager instance shared across all
   * peer/update-groups
   */
  std::shared_ptr<PolicyManager> policyManager_{nullptr};

  /*
   * Callback to check if RIB initialization is complete.
   * Used to auto-trigger initial dumps for new groups created after BGP init.
   */
  std::function<bool()> isRibInitDone_{nullptr};
};

} // namespace facebook::bgp
