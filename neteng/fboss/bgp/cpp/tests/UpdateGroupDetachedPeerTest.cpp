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

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define AdjRib_TEST_FRIENDS                                                    \
  friend class UpdateGroupDetachedPeerTest;                                    \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, ClonePackingListCreatesDeepCopy);   \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, ClonedPackingListContainsAllEntries);       \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, IsDFPReturnsTrueWhenAllConditionsMet);      \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenPLNotEmpty);   \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenVersionsMismatch);     \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      IsReadyToRejoinGroupReturnsFalseWhenConsumerNotReadyEvenIfPLEmpty);      \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      IsReadyToRejoinGroupReturnsFalseWhenPLNotEmpty);                         \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      IsReadyToRejoinGroupReturnsFalseWhenConsumerNotReady);                   \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      IsReadyToRejoinGroupReturnsFalseWhenNoConsumer);                         \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, IsReadyToRejoinGroupReturnsTrue);   \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenGroupPLEmpty); \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenNoGroup);      \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, DetachSlowPeerSetsEgressEoRWhenFlagIsTrue); \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      DetachDoesNotSetEoRsPendingWhenGroupHasNone);                            \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, RibWalkDetachCopiesEgressEoRsPending);      \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      CanAnnounceDelegatesToGroupWhenUpdateGroupEnabled);                      \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, CanAnnounceUsesGroupRrClientSetting);       \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      CollapseLiteSkipsRibVersionCheckForInitDumpPeer);                        \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      CollapsePathSkipsRibVersionCheckForInitDumpPeer);                        \
  friend class UpdateGroupDetachLifecycleTest;                                 \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest, AcceptPeerSucceedsWithMatchingEntries);  \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest, AcceptPeerFailsWithMismatchingEntries);  \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      AcceptDSPWithMixedSharedAndClonedEntries);                               \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      AcceptDSPFailsWhenPostDetachEntryNotCloned);                             \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest, CheckAndAcceptDSPPeerGroupNotReady);     \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest, CheckAndAcceptDSPPeerConsumerNotReady);  \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      CanAnnounceDelegatesToGroupWhenUpdateGroupEnabled);                      \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, CanAnnounceUsesGroupRrClientSetting);       \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      CanAnnounceEntrySkipsSuppressAsLoopWithUpdateGroup);                     \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      DetachCopiesEgressStatsAndCollapseReconciles);                           \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      CollapseDecrementsStatsOnWithdrawalDiscrepancy);                         \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      CollapseIncrementsStatsOnAnnouncementDiscrepancy);                       \
  FRIEND_TEST(UpdateGroupDetachLifecycleTest, DetachPeerSetsAllExpectedState);

#define AdjRibOutGroup_TEST_FRIENDS                                            \
  friend class UpdateGroupDetachedPeerTest;                                    \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, ShouldCloneFalseWhenPeerHasOwnEntry);       \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      ShouldCloneFalseWhenEntryAnnouncedAfterDetach);                          \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, ShouldCloneTrueWhenPeerWasSharingEntry);    \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, ShouldClonePathFalseWhenPeerHasOwnEntry);   \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      ShouldClonePathFalseWhenEntryAnnouncedAfterDetach);                      \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      ShouldClonePathTrueWhenPeerWasSharingEntry);                             \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, CopyEntryForPeerCopiesAllFields);   \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, LazyCloneNoDetachedPeersNoClone);   \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, LazyCloneClonesForSharingPeerOnly); \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, WithdrawClonesEntryToDetachedPeer); \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, WithdrawNoClonesWhenNoPeersDetached);       \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, WithdrawSkipsCloneWhenPeerHasOwnEntry);     \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      WithdrawSkipsCloneWhenEntryAnnouncedAfterDetach);                        \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, WithdrawMultiplePeersMixedCloneBehavior);   \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, WithdrawClonesEntryAddPathMode);    \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, AnnouncementClonesEntryToDetachedPeer);     \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, AnnouncementNoClonesWhenNoPeersDetached);   \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, AnnouncementSkipsCloneWhenPeerHasOwnEntry); \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      AnnouncementSkipsCloneWhenEntryAnnouncedAfterDetach);                    \
  FRIEND_TEST(UpdateGroupDetachedPeerTest, AnnouncementNoClonesForNewPrefix);  \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, AnnouncementClonesEntryAddPathMode);        \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, DetachSlowPeerSetsEgressEoRWhenFlagIsTrue); \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      DetachDoesNotSetEoRsPendingWhenGroupHasNone);                            \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest, RibWalkDetachCopiesEgressEoRsPending);      \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      CollapseLiteSkipsRibVersionCheckForInitDumpPeer);                        \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachedPeerTest,                                             \
      CollapsePathSkipsRibVersionCheckForInitDumpPeer);                        \
  friend class UpdateGroupDetachLifecycleTest;                                 \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest, AcceptPeerSucceedsWithMatchingEntries);  \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest, AcceptPeerFailsWithMismatchingEntries);  \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      CheckAndAcceptOnlyProcessesReadyToJoinPeers);                            \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      CheckAndAcceptMultiplePeersMixedResults);                                \
  FRIEND_TEST(UpdateGroupDetachLifecycleTest, AcceptDFPWithSharedEntriesOnly); \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      AcceptDSPWithMixedSharedAndClonedEntries);                               \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      AcceptDSPFailsWhenPostDetachEntryNotCloned);                             \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      DetachCopiesEgressStatsAndCollapseReconciles);                           \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      CollapseDecrementsStatsOnWithdrawalDiscrepancy);                         \
  FRIEND_TEST(                                                                 \
      UpdateGroupDetachLifecycleTest,                                          \
      CollapseIncrementsStatsOnAnnouncementDiscrepancy);                       \
  FRIEND_TEST(UpdateGroupDetachLifecycleTest, DetachPeerSetsAllExpectedState);

#include <fmt/core.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/LoggerDB.h>
#include <folly/logging/test/TestLogHandler.h>

#include <fb303/ThreadCachedServiceData.h>
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibGroup.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRibStructs.h"
#include "neteng/fboss/bgp/cpp/adjrib/ShadowRibTypes.h"
#include "neteng/fboss/bgp/cpp/changeTracker/ChangeTracker.h"
#include "neteng/fboss/bgp/cpp/changeTracker/TrackableObject.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/stats/Stats.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

namespace {
int64_t getNumSlowPeersCounter() {
  fb303::ThreadCachedServiceData::get()->publishStats();
  return fb303::ThreadCachedServiceData::get()->getCounter(
      BgpStats::kSlowPeerDetectionCount + ".sum");
}
} // namespace

using ::testing::HasSubstr;

constexpr auto kPeerIpFmt = "10.0.0.{}";

class UpdateGroupDetachedPeerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    folly::SingletonVault::singleton()->registrationComplete();
    BgpStats::initCounters();
    evb_ = std::make_unique<folly::EventBase>();
    UpdateGroupConfig ugConfig;
    ugConfig.allowSlowPeerDetach = true;
    group_ = std::make_shared<AdjRibOutGroup>(
        *evb_,
        "test_group",
        1,
        true /* enableUpdateGroup */,
        UpdateGroupKey{},
        nullptr /* shadowRibEntries */,
        nullptr /* policyManager */,
        ugConfig);
  }

  void TearDown() override {
    for (auto& peer : peers_) {
      group_->unregisterPeer(peer);
    }

    /* Drain asyncScope_ cooperatively before destroying the group */
    folly::coro::blockingWait(group_->drainAsyncScope());
    group_.reset();
    peers_.clear();
    evb_.reset();
  }

  /*
   * Create a minimal AdjRib and register it at the given bit position.
   * Uses setBitToAdjRibForTesting to avoid full registerPeer() flow.
   */
  std::shared_ptr<AdjRib> createAndRegisterPeer(uint64_t bit) {
    auto peerId = nettools::bgplib::BgpPeerId(
        folly::IPAddress(fmt::format(kPeerIpFmt, bit + 1)),
        folly::IPAddressV4("255.0.0.1").toLongHBO());
    auto adjRib = std::make_shared<AdjRib>(
        peerId,
        PeeringParams(),
        *evb_,
        ribInQ_,
        observerQ_,
        std::make_shared<folly::coro::Baton>(),
        nullptr /* policyManager */,
        std::make_shared<std::atomic<bool>>(false));
    adjRib->setGroupBitPosition(bit);
    adjRib->setUpdateGroup(group_);
    adjRib->enableUpdateGroup_ = true;
    group_->setBitToAdjRibForTesting(bit, adjRib);
    peers_.push_back(adjRib);
    return adjRib;
  }

  std::unique_ptr<folly::EventBase> evb_;
  std::shared_ptr<AdjRibOutGroup> group_;
  std::vector<std::shared_ptr<AdjRib>> peers_;
  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  MonitoredMPMCQueue<AdjRib::ObservableMessageT> observerQ_;
};

TEST_F(UpdateGroupDetachedPeerTest, InitiallyNoPeersDetached) {
  EXPECT_TRUE(group_->getDetachedPeers().empty());
}

TEST_F(UpdateGroupDetachedPeerTest, MarkPeerDetached) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  group_->markPeerDetached(adjRib0);

  const auto& peers = group_->getDetachedPeers();
  EXPECT_TRUE(peers.contains(adjRib0));
  EXPECT_FALSE(peers.contains(adjRib1));
}

TEST_F(UpdateGroupDetachedPeerTest, MarkPeerConverged) {
  auto adjRib0 = createAndRegisterPeer(0);
  group_->markPeerDetached(adjRib0);
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));

  group_->markPeerInSync(adjRib0);
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));
}

TEST_F(UpdateGroupDetachedPeerTest, MultiplePeersDetached) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto adjRib2 = createAndRegisterPeer(2);
  auto adjRib3 = createAndRegisterPeer(3);
  group_->markPeerDetached(adjRib0);
  group_->markPeerDetached(adjRib2);
  group_->markPeerDetached(adjRib3);

  const auto& peers = group_->getDetachedPeers();
  EXPECT_TRUE(peers.contains(adjRib0));
  EXPECT_FALSE(peers.contains(adjRib1));
  EXPECT_TRUE(peers.contains(adjRib2));
  EXPECT_TRUE(peers.contains(adjRib3));
}

TEST_F(UpdateGroupDetachedPeerTest, ConvergeOnePeerLeavesOthers) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  group_->markPeerDetached(adjRib0);
  group_->markPeerDetached(adjRib1);

  group_->markPeerInSync(adjRib0);

  const auto& peers = group_->getDetachedPeers();
  EXPECT_FALSE(peers.contains(adjRib0));
  EXPECT_TRUE(peers.contains(adjRib1));
}

TEST_F(UpdateGroupDetachedPeerTest, MarkDetachedIdempotent) {
  auto adjRib0 = createAndRegisterPeer(0);
  group_->markPeerDetached(adjRib0);
  group_->markPeerDetached(adjRib0);

  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));

  group_->markPeerInSync(adjRib0);
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));
}

TEST_F(UpdateGroupDetachedPeerTest, MarkConvergedIdempotent) {
  auto adjRib0 = createAndRegisterPeer(0);
  group_->markPeerInSync(adjRib0);
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));
}

TEST_F(UpdateGroupDetachedPeerTest, GetDetachedPeersReflectsState) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto adjRib2 = createAndRegisterPeer(2);
  auto adjRib3 = createAndRegisterPeer(3);
  group_->markPeerDetached(adjRib1);
  group_->markPeerDetached(adjRib3);

  const auto& peers = group_->getDetachedPeers();
  EXPECT_TRUE(peers.contains(adjRib1));
  EXPECT_TRUE(peers.contains(adjRib3));
  EXPECT_FALSE(peers.contains(adjRib0));
  EXPECT_FALSE(peers.contains(adjRib2));
}

TEST_F(UpdateGroupDetachedPeerTest, GetDetachedPeersAllCleared) {
  auto adjRib0 = createAndRegisterPeer(0);
  group_->markPeerDetached(adjRib0);
  group_->markPeerInSync(adjRib0);

  EXPECT_TRUE(group_->getDetachedPeers().empty());
}

TEST_F(UpdateGroupDetachedPeerTest, MarkPeerDetachedClearsSyncBit) {
  auto adjRib0 = createAndRegisterPeer(0);
  group_->markPeerInSync(adjRib0);
  EXPECT_TRUE(group_->isPeerInSync(0));

  group_->markPeerDetached(adjRib0);
  EXPECT_FALSE(group_->isPeerInSync(0));
}

TEST_F(UpdateGroupDetachedPeerTest, MarkPeerInSyncSetsSyncBit) {
  auto adjRib0 = createAndRegisterPeer(0);
  group_->markPeerDetached(adjRib0);
  EXPECT_FALSE(group_->isPeerInSync(0));

  group_->markPeerInSync(adjRib0);
  EXPECT_TRUE(group_->isPeerInSync(0));
}

TEST_F(UpdateGroupDetachedPeerTest, ClonePackingListCreatesDeepCopy) {
  auto adjRib = createAndRegisterPeer(0);

  // Populate group's packing list with one entry
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  attrs->setLocalPref(100);
  std::shared_ptr<const BgpPath> constAttrs = attrs;
  const folly::CIDRNetwork prefix1{folly::IPAddress("10.0.0.0"), 24};
  auto pp1 = std::make_pair(prefix1, kPlaceholderPathID);

  group_->tryUpdateAttrToPrefixMapForGroup(pp1, nullptr, constAttrs);
  ASSERT_EQ(group_->getAttrToPrefixMap().size(), 1);

  // Clone via the actual API
  group_->clonePackingListForPeer(adjRib);
  auto& detachedPl = adjRib->attrToPrefixMap_;
  ASSERT_EQ(detachedPl.size(), 1);

  // Verify the cloned packing list contains prefix1
  const auto& clonedPrefixSet = detachedPl.begin()->second;
  EXPECT_EQ(clonedPrefixSet.count(pp1), 1);

  const folly::CIDRNetwork prefix2{folly::IPAddress("20.0.0.0"), 24};
  auto pp2 = std::make_pair(prefix2, kPlaceholderPathID);
  const folly::CIDRNetwork prefix3{folly::IPAddress("30.0.0.0"), 24};
  auto pp3 = std::make_pair(prefix3, kPlaceholderPathID);

  // Modify group's packing list and verify detached is unaffected
  {
    group_->tryUpdateAttrToPrefixMapForGroup(pp2, nullptr, constAttrs);

    const auto& groupPrefixSet = group_->getAttrToPrefixMap().begin()->second;
    EXPECT_EQ(groupPrefixSet.count(pp1), 1);
    EXPECT_EQ(groupPrefixSet.count(pp2), 1);
    EXPECT_EQ(groupPrefixSet.count(pp3), 0);
    EXPECT_EQ(clonedPrefixSet.count(pp1), 1);
    EXPECT_EQ(clonedPrefixSet.count(pp2), 0);
    EXPECT_EQ(clonedPrefixSet.count(pp3), 0);
  }

  // Modify detached packing list and verify group is unaffected
  {
    BgpPathWithAfi key = detachedPl.begin()->first;
    detachedPl[key].insert(pp3);

    EXPECT_EQ(detachedPl.begin()->second.count(pp1), 1);
    EXPECT_EQ(detachedPl.begin()->second.count(pp2), 0);
    EXPECT_EQ(detachedPl.begin()->second.count(pp3), 1);
    const auto& groupPrefixSet = group_->getAttrToPrefixMap().begin()->second;
    EXPECT_EQ(groupPrefixSet.count(pp1), 1);
    EXPECT_EQ(groupPrefixSet.count(pp2), 1);
    EXPECT_EQ(groupPrefixSet.count(pp3), 0);
  }
}

TEST_F(UpdateGroupDetachedPeerTest, ClonedPackingListContainsAllEntries) {
  auto adjRib = createAndRegisterPeer(0);

  // Populate group's packing list with multiple attr groups
  auto attrs1 = std::make_shared<BgpPath>(BgpPathFields());
  attrs1->setLocalPref(100);
  std::shared_ptr<const BgpPath> constAttrs1 = attrs1;

  auto attrs2 = std::make_shared<BgpPath>(BgpPathFields());
  attrs2->setLocalPref(200);
  std::shared_ptr<const BgpPath> constAttrs2 = attrs2;

  const folly::CIDRNetwork prefix1{folly::IPAddress("10.0.0.0"), 24};
  const folly::CIDRNetwork prefix2{folly::IPAddress("20.0.0.0"), 24};
  const folly::CIDRNetwork prefix3{folly::IPAddress("30.0.0.0"), 24};

  // Two prefixes under attrs1, one under attrs2
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(prefix1, kPlaceholderPathID), nullptr, constAttrs1);
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(prefix2, kPlaceholderPathID), nullptr, constAttrs1);
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(prefix3, kPlaceholderPathID), nullptr, constAttrs2);

  ASSERT_EQ(group_->getAttrToPrefixMap().size(), 2); // 2 attr groups

  // Clone via the actual API
  group_->clonePackingListForPeer(adjRib);
  const auto& detachedPl = adjRib->attrToPrefixMap_;

  // Verify same number of attr groups
  EXPECT_EQ(detachedPl.size(), 2);

  // Verify total prefix count and shared_ptr identity
  size_t groupPrefixCount = 0;
  for (const auto& [groupKey, groupPrefixSet] : group_->getAttrToPrefixMap()) {
    groupPrefixCount += groupPrefixSet.size();
    auto it = detachedPl.find(groupKey);
    ASSERT_NE(it, detachedPl.end());
    EXPECT_EQ(groupPrefixSet.size(), it->second.size());
    // The shared_ptr<const BgpPath> in the key should point to the same object
    EXPECT_EQ(groupKey.attrs.get(), it->first.attrs.get());
  }
  EXPECT_EQ(groupPrefixCount, 3);
}

TEST_F(UpdateGroupDetachedPeerTest, DetachSinglePeerGroupSkipsDetachment) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setPeerState(PeerUpdateState::JOINED_BLOCKED);
  group_->setSyncBitForTesting(0);
  group_->markPeerBlocked(adjRib);

  auto slowPeersBefore = getNumSlowPeersCounter();

  auto& messages = subscribeToLogMessages("", folly::LogLevel::INFO);
  messages.clear();

  group_->detachSlowPeer(adjRib);

  // Peer should NOT be detached — still in sync, not diverged
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib));
  EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::JOINED_BLOCKED);

  // Verify log message
  bool foundSkipLog = false;
  for (const auto& [msg, cat] : messages) {
    if (msg.getMessage().find(
            "is the only synced member, skipping detachment") !=
        std::string::npos) {
      foundSkipLog = true;
      break;
    }
  }
  EXPECT_TRUE(foundSkipLog);

  // Counter should increment — slow peer was detected even though
  // detachment was skipped (single peer)
  EXPECT_EQ(slowPeersBefore + 1, getNumSlowPeersCounter());
}

TEST_F(UpdateGroupDetachedPeerTest, DetachPeerInMultiPeerGroup) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);

  // Set up change tracker and consumer bitmaps
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  // Create and register group consumer
  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  // Wire the tracker and consumer into the group
  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  // Publish items to the change list so the group consumer has a non-null
  // marker
  ShadowRibEntry entry1;
  entry1.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable1 =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry1));
  ShadowRibEntry entry2;
  entry2.prefix = folly::CIDRNetwork{folly::IPAddress("20.0.0.0"), 24};
  auto trackable2 =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry2));
  changeTracker->publishChange(trackable1.get());
  changeTracker->publishChange(trackable2.get());

  // Group consumer should be pended at a non-null marker
  ASSERT_NE(groupConsumer->getMarker(), nullptr);

  // Set peer 0 to JOINED_BLOCKED, peer 1 to JOINED_RUNNING
  adjRib0->setPeerState(PeerUpdateState::JOINED_BLOCKED);
  adjRib1->setPeerState(PeerUpdateState::JOINED_RUNNING);

  // Set sync bits for both peers
  group_->setSyncBitForTesting(0);
  group_->setSyncBitForTesting(1);

  // Mark peer 0 as blocked
  group_->markPeerBlocked(adjRib0);
  EXPECT_TRUE(BitmapUtils::isBitSet(group_->getBlockedBitmap(), 0));

  // Verify no detached consumer before detachment
  EXPECT_EQ(adjRib0->getDetachedConsumer(), nullptr);

  auto& messages = subscribeToLogMessages("", folly::LogLevel::INFO);
  messages.clear();

  group_->detachSlowPeer(adjRib0);

  // Peer 0 should be detached and unblocked after detachment
  EXPECT_FALSE(group_->isPeerInSync(0));
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_FALSE(BitmapUtils::isBitSet(group_->getBlockedBitmap(), 0));
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);

  // Verify detached consumer was created at the same position as group consumer
  auto detachedConsumer = adjRib0->getDetachedConsumer();
  ASSERT_NE(detachedConsumer, nullptr);
  EXPECT_EQ(detachedConsumer->getMarker(), groupConsumer->getMarker());

  // Peer 1 should be unaffected
  EXPECT_TRUE(group_->isPeerInSync(1));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib1));
  EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_EQ(adjRib1->getDetachedConsumer(), nullptr);

  // Verify detachment success log
  bool foundDetachLog = false;
  for (const auto& [msg, cat] : messages) {
    if (msg.getMessage().find("detached successfully") != std::string::npos) {
      foundDetachLog = true;
      break;
    }
  }
  EXPECT_TRUE(foundDetachLog);

  // Clean up consumers before group destruction
  adjRib0->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
  groupConsumer.reset();
}

TEST_F(UpdateGroupDetachedPeerTest, RegisterDetachedConsumerSkipsIfAlreadySet) {
  auto adjRib = createAndRegisterPeer(0);

  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  // Register once
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  auto firstConsumer = adjRib->getDetachedConsumer();
  ASSERT_NE(firstConsumer, nullptr);

  auto& messages = subscribeToLogMessages("", folly::LogLevel::WARN);
  messages.clear();

  // Register again — should be a no-op
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  EXPECT_EQ(adjRib->getDetachedConsumer(), firstConsumer);

  ASSERT_EQ(messages.size(), 1);
  EXPECT_THAT(
      messages[0].first.getMessage(),
      HasSubstr("already has a detached consumer"));

  // Clean up
  adjRib->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
}

TEST_F(UpdateGroupDetachedPeerTest, RegisterDetachedConsumerSkipsNullTracker) {
  auto adjRib = createAndRegisterPeer(0);

  std::shared_ptr<ChangeTracker<ShadowRibEntry>> nullTracker;
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;
  std::shared_ptr<AdjRibOutGroupConsumer> groupConsumer;

  auto& messages = subscribeToLogMessages("", folly::LogLevel::ERR);
  messages.clear();

  adjRib->registerDetachedConsumer(
      nullTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  EXPECT_EQ(adjRib->getDetachedConsumer(), nullptr);

  ASSERT_EQ(messages.size(), 1);
  EXPECT_THAT(
      messages[0].first.getMessage(), HasSubstr("null changeListTracker"));
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    RegisterDetachedConsumerSkipsNullGroupConsumer) {
  auto adjRib = createAndRegisterPeer(0);

  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;
  std::shared_ptr<AdjRibOutGroupConsumer> nullGroupConsumer;

  auto& messages = subscribeToLogMessages("", folly::LogLevel::ERR);
  messages.clear();

  adjRib->registerDetachedConsumer(
      changeTracker, nullGroupConsumer, addPathBitmap, nonAddPathBitmap);
  EXPECT_EQ(adjRib->getDetachedConsumer(), nullptr);

  ASSERT_EQ(messages.size(), 1);
  EXPECT_THAT(messages[0].first.getMessage(), HasSubstr("null groupConsumer"));
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    DeactivateDetachedModeProcessingNoOpWhenNull) {
  auto adjRib = createAndRegisterPeer(0);

  EXPECT_EQ(adjRib->getDetachedConsumer(), nullptr);
  adjRib->deactivateDetachedModeProcessing();
  EXPECT_EQ(adjRib->getDetachedConsumer(), nullptr);
  EXPECT_EQ(0, adjRib->getDetachedRibVersion());
}

/*
 * isDFP() tests
 */

TEST_F(UpdateGroupDetachedPeerTest, IsDFPReturnsTrueWhenAllConditionsMet) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);

  // Condition 1: detachedPackingList_ empty (default)
  ASSERT_TRUE(adjRib->attrToPrefixMap_.empty());

  // Condition 2: group and peer have same lastSeenRibVersion
  group_->setLastSeenRibVersion(42);
  adjRib->setLastSeenRibVersion(42);
  // Peer is detached (not in sync), so getLastSeenRibVersion returns
  // peer's own version
  ASSERT_FALSE(group_->isPeerInSync(0));

  // Verify per-peer table version ODS counter matches
  auto tcData = facebook::fb303::ThreadCachedServiceData::get();
  tcData->publishStats();
  EXPECT_EQ(
      42,
      tcData->getCounter(
          fmt::format(
              PeerStats::kPeerTableVersion, kEbbPlatform, kBgpcppTag, "")));

  // Condition 3: group's packing list is non-empty
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  attrs->setLocalPref(100);
  std::shared_ptr<const BgpPath> constAttrs = attrs;
  const folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(prefix, kPlaceholderPathID), nullptr, constAttrs);
  ASSERT_FALSE(group_->getAttrToPrefixMap().empty());

  EXPECT_TRUE(adjRib->isDFP());
}

TEST_F(UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenPLNotEmpty) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);

  // Set matching versions
  group_->setLastSeenRibVersion(42);
  adjRib->setLastSeenRibVersion(42);

  // Non-empty group PL
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  std::shared_ptr<const BgpPath> constAttrs = attrs;
  const folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(prefix, kPlaceholderPathID), nullptr, constAttrs);

  // But detached PL is non-empty
  group_->clonePackingListForPeer(adjRib);
  ASSERT_FALSE(adjRib->attrToPrefixMap_.empty());

  EXPECT_FALSE(adjRib->isDFP());
}

TEST_F(UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenVersionsMismatch) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);

  // Empty detached PL (default)
  ASSERT_TRUE(adjRib->attrToPrefixMap_.empty());

  // Versions differ — group has moved on
  group_->setLastSeenRibVersion(50);
  adjRib->setLastSeenRibVersion(42);

  // Verify per-peer table version ODS counter reflects peer's version
  auto tcData = facebook::fb303::ThreadCachedServiceData::get();
  tcData->publishStats();
  EXPECT_EQ(
      42,
      tcData->getCounter(
          fmt::format(
              PeerStats::kPeerTableVersion, kEbbPlatform, kBgpcppTag, "")));

  // Non-empty group PL
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  std::shared_ptr<const BgpPath> constAttrs = attrs;
  const folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(prefix, kPlaceholderPathID), nullptr, constAttrs);

  EXPECT_FALSE(adjRib->isDFP());
}

TEST_F(UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenGroupPLEmpty) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);

  // Empty detached PL and matching versions
  group_->setLastSeenRibVersion(42);
  adjRib->setLastSeenRibVersion(42);

  // But group PL is empty
  ASSERT_TRUE(group_->getAttrToPrefixMap().empty());

  EXPECT_FALSE(adjRib->isDFP());
}

TEST_F(UpdateGroupDetachedPeerTest, IsDFPReturnsFalseWhenNoGroup) {
  auto adjRib = createAndRegisterPeer(0);
  // Don't set update group — adjRibOutGroup_ remains nullptr

  EXPECT_FALSE(adjRib->isDFP());
}

/*
 * isReadyToRejoinGroup() tests
 */

TEST_F(
    UpdateGroupDetachedPeerTest,
    IsReadyToRejoinGroupReturnsFalseWhenConsumerNotReadyEvenIfPLEmpty) {
  auto adjRib = createAndRegisterPeer(0);

  // Set up change tracker and consumer
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  // Publish an item so group consumer has a pending change
  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());

  // Group consumer has pending item — not ready
  ASSERT_FALSE(groupConsumer->isReady());

  // Register detached consumer — it joins at group's position (before item)
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  auto detachedConsumer = adjRib->getDetachedConsumer();
  ASSERT_NE(detachedConsumer, nullptr);

  // Detached consumer has unconsumed items — not ready
  ASSERT_FALSE(detachedConsumer->isReady());

  // PL is empty but consumer is not ready — should return false
  ASSERT_TRUE(adjRib->attrToPrefixMap_.empty());
  EXPECT_FALSE(adjRib->isReadyToRejoinGroup());

  // Clean up
  adjRib->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    IsReadyToRejoinGroupReturnsFalseWhenPLNotEmpty) {
  auto adjRib = createAndRegisterPeer(0);

  // Set up detached consumer
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);

  // Non-empty detached PL
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  std::shared_ptr<const BgpPath> constAttrs = attrs;
  const folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(prefix, kPlaceholderPathID), nullptr, constAttrs);
  group_->clonePackingListForPeer(adjRib);
  ASSERT_FALSE(adjRib->attrToPrefixMap_.empty());

  EXPECT_FALSE(adjRib->isReadyToRejoinGroup());

  // Clean up
  adjRib->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    IsReadyToRejoinGroupReturnsFalseWhenConsumerNotReady) {
  auto adjRib = createAndRegisterPeer(0);

  // Set up change tracker with pending items
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  // Publish items AFTER group consumer registers, so group consumer
  // has pending items
  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());

  // Register detached consumer — it joins at group's position (before the
  // published item), so it has unconsumed items
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  auto detachedConsumer = adjRib->getDetachedConsumer();
  ASSERT_NE(detachedConsumer, nullptr);
  ASSERT_FALSE(detachedConsumer->isReady());

  // Detached PL is empty
  ASSERT_TRUE(adjRib->attrToPrefixMap_.empty());

  EXPECT_FALSE(adjRib->isReadyToRejoinGroup());

  // Clean up
  adjRib->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    IsReadyToRejoinGroupReturnsFalseWhenNoConsumer) {
  auto adjRib = createAndRegisterPeer(0);

  // No detached consumer registered
  ASSERT_EQ(adjRib->getDetachedConsumer(), nullptr);
  ASSERT_TRUE(adjRib->attrToPrefixMap_.empty());

  EXPECT_FALSE(adjRib->isReadyToRejoinGroup());
}

TEST_F(UpdateGroupDetachedPeerTest, IsReadyToRejoinGroupReturnsTrue) {
  auto adjRib = createAndRegisterPeer(0);

  // Set up change tracker and consumer
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  // Publish and consume an item so group consumer reaches ready state
  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());
  groupConsumer->iterateChanges();
  ASSERT_TRUE(groupConsumer->isReady());

  // Register detached consumer — joins at group's ready position
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  auto detachedConsumer = adjRib->getDetachedConsumer();
  ASSERT_NE(detachedConsumer, nullptr);

  ASSERT_TRUE(detachedConsumer->isReady());

  // PL is empty and consumer is ready — should return true
  ASSERT_TRUE(adjRib->attrToPrefixMap_.empty());
  EXPECT_TRUE(adjRib->isReadyToRejoinGroup());

  // Clean up
  adjRib->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
}

/*
 * activateDetachedModeProcessing() tests
 */

TEST_F(UpdateGroupDetachedPeerTest, ActivateDSPTransitionsToReadyToJoin) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);
  adjRib->setPeerState(PeerUpdateState::DETACHED_RUNNING);

  // Set up change tracker and consumer so isReadyToRejoinGroup() can pass
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();
  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  // Publish a CL item and consume it so group consumer is ready
  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());
  groupConsumer->iterateChanges();
  EXPECT_TRUE(groupConsumer->isReady());

  // Register detached consumer — joins at group's ready position
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  EXPECT_TRUE(adjRib->getDetachedConsumer()->isReady());

  // Activate detached mode processing — schedules sendBgpUpdates on evb
  adjRib->activateDetachedModeProcessing();

  // Pump the evb so sendBgpUpdates runs and DFP check triggers
  evb_->loopOnce();

  // DSP: peer and group both ready -> checkAndAcceptDSPPeer accepts immediately
  EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(0));

  // Clean up
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
}

TEST_F(UpdateGroupDetachedPeerTest, ActivateDSPStartsTimerLoop) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);
  adjRib->setPeerState(PeerUpdateState::DETACHED_RUNNING);

  // Set up change tracker and consumer for CL consumption
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();
  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  // Publish an item but DON'T consume it — group consumer stays pended.
  // Detached consumer joins pended too (not ready = DSP).
  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());

  // Register detached consumer at group's position (pended, not ready)
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);

  // Simulate prior block history
  adjRib->getPeerBlockInfo().blockCount = 3;

  // Activate detached mode processing — schedules sendBgpUpdates
  adjRib->activateDetachedModeProcessing();

  // Block info is NOT reset on activation — it's reset on rejoin instead,
  // so frequency-based slow peer detection is preserved while detached.
  EXPECT_EQ(adjRib->getPeerBlockInfo().blockCount, 3);

  // Pump the evb so sendBgpUpdates runs and schedules the CL timer
  evb_->loopOnce();

  // Peer should NOT transition to DETACHED_READY_TO_JOIN yet
  EXPECT_NE(adjRib->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);

  // Clean up
  adjRib->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
}

TEST_F(UpdateGroupDetachedPeerTest, ActivateDFPTransitionsViaIsDFPPath) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);
  adjRib->setPeerState(PeerUpdateState::DETACHED_RUNNING);

  // Set up change tracker and consumer
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();
  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  // Publish an item but DON'T consume it — group consumer stays pended.
  // This means isReadyToRejoinGroup() will return false (consumer not ready).
  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());

  // Register detached consumer at group's position (pended, not ready)
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);
  EXPECT_FALSE(adjRib->getDetachedConsumer()->isReady());

  // Set up DFP conditions:
  //   1. PL empty (default)
  //   2. Matching versions between group and peer
  //   3. Group PL non-empty
  group_->setLastSeenRibVersion(42);
  adjRib->setLastSeenRibVersion(42);
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  attrs->setLocalPref(100);
  std::shared_ptr<const BgpPath> constAttrs = attrs;
  group_->tryUpdateAttrToPrefixMapForGroup(
      std::make_pair(
          folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24},
          kPlaceholderPathID),
      nullptr,
      constAttrs);

  EXPECT_FALSE(adjRib->isReadyToRejoinGroup());
  EXPECT_TRUE(adjRib->isDFP());

  // Activate detached mode processing — schedules sendBgpUpdates on evb
  adjRib->activateDetachedModeProcessing();

  // Pump the evb so sendBgpUpdates runs and isDFP() triggers transition
  evb_->loopOnce();

  // DFP path: isDFP() returns true -> DETACHED_READY_TO_JOIN with flag
  EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);
  EXPECT_TRUE(adjRib->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER));

  // Clean up
  adjRib->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
}

TEST_F(UpdateGroupDetachedPeerTest, ActivateDSPLogsErrorWithNoConsumer) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);
  adjRib->setPeerState(PeerUpdateState::DETACHED_RUNNING);

  // No detached consumer — isReadyToRejoinGroup() will return false

  adjRib->activateDetachedModeProcessing();

  // Pump the evb so sendBgpUpdates runs and hits the no-consumer check
  evb_->loopOnce();

  // Peer should remain in DETACHED_RUNNING (no transition without consumer)
  EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::DETACHED_RUNNING);
}

/*
 * deactivateDetachedModeProcessing() tests
 */

TEST_F(UpdateGroupDetachedPeerTest, DeactivateNoOpWhenNoTimer) {
  auto adjRib = createAndRegisterPeer(0);

  // No timer set — should be a no-op, no crash
  adjRib->deactivateDetachedModeProcessing();
}

/*
 * markPeerUnblocked() with detached peer tests
 */

TEST_F(
    UpdateGroupDetachedPeerTest,
    MarkPeerUnblockedTriggersDetachedProcessing) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setUpdateGroup(group_);

  // Set up change tracker and consumer so isReadyToRejoinGroup() can pass
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();
  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  // Publish a CL item and consume it so group consumer is ready
  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());
  groupConsumer->iterateChanges();

  // Register detached consumer — joins at group's ready position
  adjRib->registerDetachedConsumer(
      changeTracker, groupConsumer, addPathBitmap, nonAddPathBitmap);

  // Set peer to DETACHED_BLOCKED state
  adjRib->setPeerState(PeerUpdateState::DETACHED_BLOCKED);

  // Unblock via markPeerUnblocked — triggers activateDetachedModeProcessing
  group_->markPeerUnblocked(adjRib);

  // Pump the evb so sendBgpUpdates runs and DFP check triggers
  evb_->loopOnce();

  // DSP: peer and group both ready -> checkAndAcceptDSPPeer accepts immediately
  EXPECT_EQ(adjRib->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(0));

  // Clean up
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
}

/*
 * RIB-OUT Lazy Clone via Group Mutation
 *
 * shouldCloneLiteForPeer() tests — 3-case clone decision algorithm.
 */

TEST_F(UpdateGroupDetachedPeerTest, ShouldCloneFalseWhenPeerHasOwnEntry) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib->getPeerOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, peerOwnerKey, kPlaceholderPathID);

  adjRib->setDetachedRibVersion(50);

  auto radixNodeItr =
      group_->getRadixNodeItrFromLiteTree(group_->LiteTree_, kV4Prefix1);
  EXPECT_FALSE(group_->shouldCloneLiteForPeer(
      radixNodeItr, adjRib, groupEntry->getRibVersion()));
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    ShouldCloneFalseWhenEntryAnnouncedAfterDetach) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(100);

  adjRib->setDetachedRibVersion(50);

  auto radixNodeItr =
      group_->getRadixNodeItrFromLiteTree(group_->LiteTree_, kV4Prefix1);
  EXPECT_FALSE(group_->shouldCloneLiteForPeer(
      radixNodeItr, adjRib, groupEntry->getRibVersion()));
}

TEST_F(UpdateGroupDetachedPeerTest, ShouldCloneTrueWhenPeerWasSharingEntry) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  adjRib->setDetachedRibVersion(50);

  auto radixNodeItr =
      group_->getRadixNodeItrFromLiteTree(group_->LiteTree_, kV4Prefix1);
  EXPECT_TRUE(group_->shouldCloneLiteForPeer(
      radixNodeItr, adjRib, groupEntry->getRibVersion()));
}

/*
 * copyEntryForPeer() tests
 */

TEST_F(UpdateGroupDetachedPeerTest, CopyEntryForPeerCopiesAllFields) {
  auto adjRib = createAndRegisterPeer(0);
  // Register a second peer so the group has >1 member and copyEntryForPeer
  // stores the cloned entry under the peer owner key (not the group key).
  auto adjRib1 = createAndRegisterPeer(1);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib->getPeerOwnerKey();

  // Set up a group entry with all fields populated
  auto preOutAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto postAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 0, 0, 0));
  std::shared_ptr<const BgpPath> constPreOut = preOutAttrs;
  std::shared_ptr<const BgpPath> constPost = postAttrs;

  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPreOut(constPreOut);
  groupEntry->setPostAttr(constPost);
  groupEntry->setPostOutPolicy("test_policy");
  groupEntry->setRibVersion(42);
  groupEntry->setStale(true);

  // Clone the entry to the peer
  group_->copyEntryForPeer(
      kV4Prefix1, kPlaceholderPathID, adjRib, peerOwnerKey, groupEntry);

  // Verify peer entry was created with all fields copied
  auto peerEntry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey);
  ASSERT_NE(peerEntry, nullptr);
  EXPECT_NE(peerEntry, groupEntry);
  EXPECT_EQ(peerEntry->getPreOut(), groupEntry->getPreOut());
  EXPECT_EQ(peerEntry->getPostAttr(), groupEntry->getPostAttr());
  EXPECT_EQ(peerEntry->getPostOutPolicy(), groupEntry->getPostOutPolicy());
  EXPECT_EQ(peerEntry->getRibVersion(), 42);
  EXPECT_EQ(peerEntry->flags_, groupEntry->flags_);
  EXPECT_TRUE(peerEntry->isStale());
}

TEST_F(UpdateGroupDetachedPeerTest, CopyEntryForPeerUsesGroupKeyWhenOnlyPeer) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib->getPeerOwnerKey();

  AdjRibEntry srcEntry(0);
  srcEntry.setRibVersion(42);

  // Caller passes the group owner key when the peer is the only member.
  group_->copyEntryForPeer(
      kV4Prefix1, kPlaceholderPathID, adjRib, groupOwnerKey, &srcEntry);

  auto entry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, groupOwnerKey);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->getRibVersion(), 42);

  // Should NOT exist under peer owner key
  auto peerEntry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey);
  EXPECT_EQ(peerEntry, nullptr);
}

/*
 * lazyCloneLiteForDetachedPeers() tests
 */

TEST_F(UpdateGroupDetachedPeerTest, LazyCloneNoDetachedPeersNoClone) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib->getPeerOwnerKey();

  // No detached peers
  ASSERT_TRUE(group_->getDetachedPeers().empty());

  // Set up a group entry
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPostAttr(std::shared_ptr<const BgpPath>(attrs));
  groupEntry->setRibVersion(10);

  // Call lazyCloneLiteForDetachedPeers — should be a no-op
  auto radixNodeItr =
      group_->getRadixNodeItrFromLiteTree(group_->LiteTree_, kV4Prefix1);
  group_->lazyCloneLiteForDetachedPeers(
      kV4Prefix1, kPlaceholderPathID, radixNodeItr, groupEntry);

  // No peer entry should have been created
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);
}

TEST_F(UpdateGroupDetachedPeerTest, LazyCloneClonesForSharingPeerOnly) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey0 = adjRib0->getPeerOwnerKey();
  auto peerOwnerKey1 = adjRib1->getPeerOwnerKey();

  // Both peers are detached
  group_->markPeerDetached(adjRib0);
  group_->markPeerDetached(adjRib1);

  // Set up a group entry that predates both peers' detach versions
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPostAttr(std::shared_ptr<const BgpPath>(attrs));
  groupEntry->setRibVersion(10);

  adjRib0->setDetachedRibVersion(50);
  adjRib1->setDetachedRibVersion(50);

  // Peer 1 already has its own entry — should NOT get a clone
  group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, peerOwnerKey1, kPlaceholderPathID);

  // Call lazyCloneLiteForDetachedPeers
  auto radixNodeItr =
      group_->getRadixNodeItrFromLiteTree(group_->LiteTree_, kV4Prefix1);
  group_->lazyCloneLiteForDetachedPeers(
      kV4Prefix1, kPlaceholderPathID, radixNodeItr, groupEntry);

  // Peer 0 was sharing the entry → should get a clone
  auto peerEntry0 =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey0);
  ASSERT_NE(peerEntry0, nullptr);
  EXPECT_EQ(peerEntry0->getPostAttr(), groupEntry->getPostAttr());
  EXPECT_EQ(peerEntry0->getRibVersion(), groupEntry->getRibVersion());

  // Peer 1 already had its own entry — should NOT get overwritten
  // (shouldCloneLiteForPeer returns false for Case 1)
  auto peerEntry1 =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey1);
  ASSERT_NE(peerEntry1, nullptr);
  // Peer 1's entry should have no postAttr (it was empty when we added it)
  EXPECT_EQ(peerEntry1->getPostAttr(), nullptr);
}

/**
 * Group withdrawal with a detached peer that was sharing the entry.
 * The entry should be cloned to the peer before being removed from the group.
 */
TEST_F(UpdateGroupDetachedPeerTest, WithdrawClonesEntryToDetachedPeer) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  // Set up group entry with ribVersion before peer detached
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPreOut(attrs);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  // Detach peer 0 at ribVersion 50 (> entry's 10, so peer was sharing it)
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // Peer should not have its own entry yet
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);

  // Withdraw the prefix from the group
  group_->processGroupRibWithdraw(kV4Prefix1, kPlaceholderPathID);

  // Peer should now have a cloned entry
  auto peerEntry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey);
  ASSERT_NE(peerEntry, nullptr);
  EXPECT_EQ(peerEntry->getPreOut(), attrs);
  EXPECT_EQ(peerEntry->getPostAttr(), attrs);
  EXPECT_EQ(peerEntry->getRibVersion(), 10);
}

/*
 * Group withdrawal with no detached peers. No clone should happen.
 */
TEST_F(UpdateGroupDetachedPeerTest, WithdrawNoClonesWhenNoPeersDetached) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPreOut(attrs);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  // No peers detached
  EXPECT_TRUE(group_->getDetachedPeers().empty());

  group_->processGroupRibWithdraw(kV4Prefix1, kPlaceholderPathID);

  // No peer entry should exist
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);
}

/*
 * Group withdrawal skips clone when detached peer already has its own entry.
 */
TEST_F(UpdateGroupDetachedPeerTest, WithdrawSkipsCloneWhenPeerHasOwnEntry) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto peerAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));

  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPreOut(attrs);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  // Peer already has its own entry (diverged earlier)
  auto existingPeerEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, peerOwnerKey, kPlaceholderPathID);
  existingPeerEntry->setPreOut(peerAttrs);
  existingPeerEntry->setPostAttr(peerAttrs);

  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  group_->processGroupRibWithdraw(kV4Prefix1, kPlaceholderPathID);

  // Peer entry should still have its original attrs (not overwritten)
  auto peerEntry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey);
  ASSERT_NE(peerEntry, nullptr);
  EXPECT_EQ(peerEntry->getPostAttr(), peerAttrs);
}

/*
 * Group withdrawal skips clone when entry was announced after peer detached.
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    WithdrawSkipsCloneWhenEntryAnnouncedAfterDetach) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPreOut(attrs);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(100); // Announced after peer detached

  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50); // Detached at version 50

  group_->processGroupRibWithdraw(kV4Prefix1, kPlaceholderPathID);

  // No clone — peer never saw this entry
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);
}

/*
 * Multiple peers in different states during group withdrawal:
 * - Peer 1 (bit 0): detached, no own entry → gets clone
 * - Peer 2 (bit 1): detached, already has own entry → no clone (keeps original)
 * - Peer 3 (bit 2): not detached (joined) → no clone
 */
TEST_F(UpdateGroupDetachedPeerTest, WithdrawMultiplePeersMixedCloneBehavior) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto adjRib2 = createAndRegisterPeer(2);

  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey0 = adjRib0->getPeerOwnerKey();
  auto peerOwnerKey1 = adjRib1->getPeerOwnerKey();
  auto peerOwnerKey2 = adjRib2->getPeerOwnerKey();

  auto groupAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto peer1Attrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));

  // Set up group entry
  auto groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPreOut(groupAttrs);
  groupEntry->setPostAttr(groupAttrs);
  groupEntry->setRibVersion(10);

  // Peer 0: detached, no own entry → should get clone
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // Peer 1: detached, already has own entry → should NOT get clone
  group_->markPeerDetached(adjRib1);
  adjRib1->setDetachedRibVersion(50);
  auto existingPeer1Entry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, peerOwnerKey1, kPlaceholderPathID);
  existingPeer1Entry->setPreOut(peer1Attrs);
  existingPeer1Entry->setPostAttr(peer1Attrs);

  // Peer 2: not detached → should NOT get clone

  group_->processGroupRibWithdraw(kV4Prefix1, kPlaceholderPathID);

  // Peer 0: should have a cloned entry with group attrs
  auto peer0Entry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey0);
  ASSERT_NE(peer0Entry, nullptr);
  EXPECT_EQ(peer0Entry->getPreOut(), groupAttrs);
  EXPECT_EQ(peer0Entry->getPostAttr(), groupAttrs);
  EXPECT_EQ(peer0Entry->getRibVersion(), 10);

  // Peer 1: should still have its original entry (not overwritten)
  auto peer1Entry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey1);
  ASSERT_NE(peer1Entry, nullptr);
  EXPECT_EQ(peer1Entry->getPostAttr(), peer1Attrs);

  // Peer 2: should NOT have any entry
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey2),
      nullptr);
}

/**
 * Group withdrawal with sendAddPath=true exercises the PathTree code path
 * in copyEntryForPeer and lazyClonePathForDetachedPeers.
 */
TEST_F(UpdateGroupDetachedPeerTest, WithdrawClonesEntryAddPathMode) {
  group_->groupKey_.afiIpv4Negotiated = true;
  group_->groupKey_.sendAddPath = true;
  auto adjRib0 = createAndRegisterPeer(0);
  // Register a second peer so copyEntryForPeer uses peer owner key.
  auto adjRib1 = createAndRegisterPeer(1);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  constexpr uint32_t kPathId = 42;

  // Set up group entry in PathTree
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto groupEntry = group_->addToPathTree(
      group_->PathTree_, kV4Prefix1, groupOwnerKey, kPathId);
  groupEntry->setPreOut(attrs);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  // Detach peer 0 at ribVersion 50 (> entry's 10, so peer was sharing it)
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // Peer should not have its own entry yet
  EXPECT_EQ(
      group_->getFromPathTree(
          group_->PathTree_, kV4Prefix1, peerOwnerKey, kPathId),
      nullptr);

  // Withdraw the prefix from the group
  group_->processGroupRibWithdraw(kV4Prefix1, kPathId);

  // Peer should now have a cloned entry in PathTree
  auto peerEntry = group_->getFromPathTree(
      group_->PathTree_, kV4Prefix1, peerOwnerKey, kPathId);
  ASSERT_NE(peerEntry, nullptr);
  EXPECT_EQ(peerEntry->getPreOut(), attrs);
  EXPECT_EQ(peerEntry->getPostAttr(), attrs);
  EXPECT_EQ(peerEntry->getRibVersion(), 10);
}

/*
 * shouldClonePathForPeer() tests — PathTree (add-path) variant.
 */

TEST_F(UpdateGroupDetachedPeerTest, ShouldClonePathFalseWhenPeerHasOwnEntry) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib->getPeerOwnerKey();

  constexpr uint32_t kPathId = 42;
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  auto groupEntry = group_->addToPathTree(
      group_->PathTree_, kV4Prefix1, groupOwnerKey, kPathId);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  group_->addToPathTree(group_->PathTree_, kV4Prefix1, peerOwnerKey, kPathId);

  adjRib->setDetachedRibVersion(50);

  auto radixNodeItr =
      group_->getRadixNodeItrFromPathTree(group_->PathTree_, kV4Prefix1);
  EXPECT_FALSE(group_->shouldClonePathForPeer(
      radixNodeItr, kPathId, adjRib, groupEntry->getRibVersion()));
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    ShouldClonePathFalseWhenEntryAnnouncedAfterDetach) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  constexpr uint32_t kPathId = 42;
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  auto groupEntry = group_->addToPathTree(
      group_->PathTree_, kV4Prefix1, groupOwnerKey, kPathId);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(100);

  adjRib->setDetachedRibVersion(50);

  auto radixNodeItr =
      group_->getRadixNodeItrFromPathTree(group_->PathTree_, kV4Prefix1);
  EXPECT_FALSE(group_->shouldClonePathForPeer(
      radixNodeItr, kPathId, adjRib, groupEntry->getRibVersion()));
}

TEST_F(
    UpdateGroupDetachedPeerTest,
    ShouldClonePathTrueWhenPeerWasSharingEntry) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  constexpr uint32_t kPathId = 42;
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  auto groupEntry = group_->addToPathTree(
      group_->PathTree_, kV4Prefix1, groupOwnerKey, kPathId);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(10);

  adjRib->setDetachedRibVersion(50);

  auto radixNodeItr =
      group_->getRadixNodeItrFromPathTree(group_->PathTree_, kV4Prefix1);
  EXPECT_TRUE(group_->shouldClonePathForPeer(
      radixNodeItr, kPathId, adjRib, groupEntry->getRibVersion()));
}

/*
 * Announcement path lazy clone tests.
 * These verify that processRibAnnouncedEntryForGroup clones the existing
 * group entry to detached peers before mutating it with new attributes.
 */

/**
 * Re-announcement of a prefix clones old state to a detached peer that was
 * sharing the group entry, then updates the group entry with new attrs.
 */
TEST_F(UpdateGroupDetachedPeerTest, AnnouncementClonesEntryToDetachedPeer) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  // First announcement — establishes group entry at ribVersion 10
  auto oldAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  oldAttrs->publish();
  RibOutAnnouncementEntry entry1(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      oldAttrs);
  entry1.ribVersion = 10;
  group_->processRibAnnouncedEntryForGroup(entry1);

  // Detach peer 0 at ribVersion 50 (> entry's 10, so peer was sharing it)
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // Peer should not have its own entry yet
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);

  // Re-announce same prefix with different attrs at ribVersion 60
  auto newAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));
  newAttrs->publish();
  RibOutAnnouncementEntry entry2(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      newAttrs);
  entry2.ribVersion = 60;
  group_->processRibAnnouncedEntryForGroup(entry2);

  // Peer should now have a cloned entry preserving the OLD state
  auto peerEntry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey);
  ASSERT_NE(peerEntry, nullptr);
  EXPECT_EQ(peerEntry->getPreOut(), oldAttrs);
  EXPECT_EQ(peerEntry->getRibVersion(), 10);

  // Group entry should have the NEW state
  auto groupEntry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, groupOwnerKey);
  ASSERT_NE(groupEntry, nullptr);
  EXPECT_EQ(groupEntry->getPreOut(), newAttrs);
  EXPECT_EQ(groupEntry->getRibVersion(), 60);
}

/*
 * Re-announcement with no detached peers. No clone should happen.
 */
TEST_F(UpdateGroupDetachedPeerTest, AnnouncementNoClonesWhenNoPeersDetached) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  // First announcement
  auto oldAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  oldAttrs->publish();
  RibOutAnnouncementEntry entry1(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      oldAttrs);
  entry1.ribVersion = 10;
  group_->processRibAnnouncedEntryForGroup(entry1);

  // No peers detached
  EXPECT_TRUE(group_->getDetachedPeers().empty());

  // Re-announce with different attrs
  auto newAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));
  newAttrs->publish();
  RibOutAnnouncementEntry entry2(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      newAttrs);
  entry2.ribVersion = 20;
  group_->processRibAnnouncedEntryForGroup(entry2);

  // No peer entry should exist
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);
}

/*
 * Re-announcement skips clone when detached peer already has its own entry.
 */
TEST_F(UpdateGroupDetachedPeerTest, AnnouncementSkipsCloneWhenPeerHasOwnEntry) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  // First announcement
  auto oldAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  oldAttrs->publish();
  RibOutAnnouncementEntry entry1(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      oldAttrs);
  entry1.ribVersion = 10;
  group_->processRibAnnouncedEntryForGroup(entry1);

  // Peer already has its own entry (diverged earlier)
  auto peerAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(3, 3, 0, 0));
  auto existingPeerEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, peerOwnerKey, kPlaceholderPathID);
  existingPeerEntry->setPreOut(peerAttrs);
  existingPeerEntry->setPostAttr(peerAttrs);

  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // Re-announce with different attrs
  auto newAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));
  newAttrs->publish();
  RibOutAnnouncementEntry entry2(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      newAttrs);
  entry2.ribVersion = 60;
  group_->processRibAnnouncedEntryForGroup(entry2);

  // Peer entry should still have its original attrs (not overwritten by clone)
  auto peerEntry =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey);
  ASSERT_NE(peerEntry, nullptr);
  EXPECT_EQ(peerEntry->getPostAttr(), peerAttrs);
}

/*
 * Re-announcement skips clone when the first announcement happened after
 * the peer detached (peer never saw this prefix).
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    AnnouncementSkipsCloneWhenEntryAnnouncedAfterDetach) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  // Detach peer 0 at ribVersion 50
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // First announcement at ribVersion 100 (after detach)
  auto oldAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  oldAttrs->publish();
  RibOutAnnouncementEntry entry1(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      oldAttrs);
  entry1.ribVersion = 100;
  group_->processRibAnnouncedEntryForGroup(entry1);

  // Re-announce with different attrs at ribVersion 110
  auto newAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));
  newAttrs->publish();
  RibOutAnnouncementEntry entry2(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      newAttrs);
  entry2.ribVersion = 110;
  group_->processRibAnnouncedEntryForGroup(entry2);

  // No clone — peer never saw this entry (both versions > detachedRibVersion)
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);
}

/*
 * First announcement of a brand new prefix with detached peers.
 * No lazy clone needed since there's no existing entry to preserve.
 */
TEST_F(UpdateGroupDetachedPeerTest, AnnouncementNoClonesForNewPrefix) {
  group_->groupKey_.afiIpv4Negotiated = true;
  auto adjRib0 = createAndRegisterPeer(0);
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  // Detach peer 0 but don't create any group entries
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // First announcement — creates a fresh entry, no clone needed
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  attrs->publish();
  RibOutAnnouncementEntry entry(
      kV4Prefix1,
      kPlaceholderPathID,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      attrs);
  entry.ribVersion = 100;
  group_->processRibAnnouncedEntryForGroup(entry);

  // No peer entry should exist (nothing to clone from)
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);
}

/**
 * Re-announcement with sendAddPath=true exercises the PathTree code path.
 */
TEST_F(UpdateGroupDetachedPeerTest, AnnouncementClonesEntryAddPathMode) {
  group_->groupKey_.afiIpv4Negotiated = true;
  group_->groupKey_.sendAddPath = true;
  auto adjRib0 = createAndRegisterPeer(0);
  // Register a second peer so copyEntryForPeer uses peer owner key.
  auto adjRib1 = createAndRegisterPeer(1);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();

  constexpr uint32_t kPathId = 42;

  // First announcement at ribVersion 10
  auto oldAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  oldAttrs->publish();
  RibOutAnnouncementEntry entry1(
      kV4Prefix1,
      kPathId,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      oldAttrs);
  entry1.ribVersion = 10;
  group_->processRibAnnouncedEntryForGroup(entry1);

  // Detach peer 0 at ribVersion 50 (> entry's 10, so peer was sharing it)
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  // Peer should not have its own entry yet
  EXPECT_EQ(
      group_->getFromPathTree(
          group_->PathTree_, kV4Prefix1, peerOwnerKey, kPathId),
      nullptr);

  // Re-announce with different attrs at ribVersion 60
  auto newAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));
  newAttrs->publish();
  RibOutAnnouncementEntry entry2(
      kV4Prefix1,
      kPathId,
      TinyPeerInfo(
          folly::IPAddress("1.1.1.1"), 65000, 1, BgpSessionType::EBGP, false),
      newAttrs);
  entry2.ribVersion = 60;
  group_->processRibAnnouncedEntryForGroup(entry2);

  // Peer should now have a cloned entry preserving the OLD state
  auto peerEntry = group_->getFromPathTree(
      group_->PathTree_, kV4Prefix1, peerOwnerKey, kPathId);
  ASSERT_NE(peerEntry, nullptr);
  EXPECT_EQ(peerEntry->getPreOut(), oldAttrs);
  EXPECT_EQ(peerEntry->getRibVersion(), 10);

  // Group entry should have the NEW state
  auto groupEntry = group_->getFromPathTree(
      group_->PathTree_, kV4Prefix1, groupOwnerKey, kPathId);
  ASSERT_NE(groupEntry, nullptr);
  EXPECT_EQ(groupEntry->getPreOut(), newAttrs);
  EXPECT_EQ(groupEntry->getRibVersion(), 60);
}

/*
 * Lifecycle tests for slow peer detachment and rejoin flow.
 *
 * These tests exercise the full lifecycle:
 *   peer joins → blocks → detaches → unblocks → DFP/DSP → ready to rejoin
 *
 * Uses a richer fixture with change tracker infrastructure pre-wired.
 */
class UpdateGroupDetachLifecycleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    folly::SingletonVault::singleton()->registrationComplete();
    evb_ = std::make_unique<folly::EventBase>();
    UpdateGroupConfig ugConfig;
    ugConfig.allowSlowPeerDetach = true;
    UpdateGroupKey ugKey;
    ugKey.afiIpv4Negotiated = true;
    ugKey.afiIpv6Negotiated = true;
    ugKey.sessionType = BgpSessionType::EBGP;
    group_ = std::make_shared<AdjRibOutGroup>(
        *evb_,
        "test_group",
        1,
        true /* enableUpdateGroup */,
        ugKey,
        nullptr /* shadowRibEntries */,
        nullptr /* policyManager */,
        ugConfig);

    // Set up change tracker infrastructure
    changeTracker_ =
        std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");

    groupConsumer_ = std::make_shared<AdjRibOutGroupConsumer>(
        changeTracker_,
        group_,
        "test_group_consumer",
        *evb_,
        addPathBitmap_,
        nonAddPathBitmap_);
    groupConsumer_->registerWithTracker();
    groupConsumer_->setBitmap();

    group_->setChangeListConsumer(groupConsumer_);
    group_->setChangeListTracker(
        changeTracker_, addPathBitmap_, nonAddPathBitmap_);
  }

  void TearDown() override {
    for (auto& peer : peers_) {
      peer->deactivateDetachedModeProcessing();
      peer->resetSlowPeerDurationTimer();
      peer->setUpdateGroup(nullptr);
    }
    groupConsumer_->resetBitmap();
    groupConsumer_->terminate();
    groupConsumer_->deregisterFromTracker();
    group_->resetChangeListConsumer();
    groupConsumer_.reset();

    /* Drain asyncScope_ cooperatively before destroying the group */
    folly::coro::blockingWait(group_->drainAsyncScope());
    group_.reset();
    peers_.clear();
    evb_.reset();
  }

  std::shared_ptr<AdjRib> createAndRegisterPeer(uint64_t bit) {
    auto peerId = nettools::bgplib::BgpPeerId(
        folly::IPAddress("10.0.0.1"),
        folly::IPAddressV4("255.0.0.1").toLongHBO());
    auto adjRib = std::make_shared<AdjRib>(
        peerId,
        PeeringParams(),
        *evb_,
        ribInQ_,
        observerQ_,
        std::make_shared<folly::coro::Baton>(),
        nullptr /* policyManager */,
        std::make_shared<std::atomic<bool>>(false));
    adjRib->setGroupBitPosition(bit);
    adjRib->setUpdateGroup(group_);
    adjRib->enableUpdateGroup_ = true;
    group_->setBitToAdjRibForTesting(bit, adjRib);
    peers_.push_back(adjRib);
    return adjRib;
  }

  /*
   * Set up a peer as JOINED_RUNNING with sync bit set.
   */
  void setUpJoinedRunningPeer(
      const std::shared_ptr<AdjRib>& adjRib,
      uint64_t bit) {
    adjRib->setPeerState(PeerUpdateState::JOINED_RUNNING);
    group_->setSyncBitForTesting(bit);
  }

  /*
   * Publish a change item to the change tracker so the group consumer
   * has a non-null marker.
   */
  TrackableObject<ShadowRibEntry>* publishChangeItem(
      const folly::CIDRNetwork& prefix) {
    auto entry = std::make_unique<ShadowRibEntry>();
    entry->prefix = prefix;
    auto trackable =
        std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(*entry));
    auto* ptr = trackable.get();
    trackableObjects_.push_back(std::move(trackable));
    changeTracker_->publishChange(ptr);
    return ptr;
  }

  /*
   * Set the group consumer to ready state so groupAtEndOfCL is true.
   * Required for DSP peers to be eligible in checkAndAcceptReadyToJoinPeers.
   */
  void setGroupConsumerReady() {
    groupConsumer_->setMarker(nullptr);
  }

  /*
   * Set up a peer's CL consumer in ready state so isReadyToRejoinGroup()
   * returns true. Required for DSP peers going through
   * checkAndAcceptReadyToJoinPeers.
   */
  void setUpReadyPeerConsumer(const std::shared_ptr<AdjRib>& adjRib) {
    auto peerConsumer = std::make_shared<AdjRibOutConsumer>(
        changeTracker_,
        adjRib,
        "peer_consumer",
        *evb_,
        addPathBitmap_,
        nonAddPathBitmap_);
    adjRib->setChangeListConsumer(peerConsumer);
    peerConsumer->setMarker(nullptr);
  }

  /*
   * Add a prefix to the group packing list.
   */
  void addPrefixToGroupPL(
      const folly::CIDRNetwork& prefix,
      uint32_t localPref = 100) {
    auto attrs = std::make_shared<BgpPath>(BgpPathFields());
    attrs->setLocalPref(localPref);
    std::shared_ptr<const BgpPath> constAttrs = attrs;
    group_->tryUpdateAttrToPrefixMapForGroup(
        std::make_pair(prefix, kPlaceholderPathID), nullptr, constAttrs);
  }

  std::unique_ptr<folly::EventBase> evb_;
  std::shared_ptr<AdjRibOutGroup> group_;
  std::shared_ptr<ChangeTracker<ShadowRibEntry>> changeTracker_;
  std::shared_ptr<AdjRibOutGroupConsumer> groupConsumer_;
  ConsumerBitmap addPathBitmap_;
  ConsumerBitmap nonAddPathBitmap_;
  std::vector<std::shared_ptr<AdjRib>> peers_;
  std::vector<std::unique_ptr<TrackableObject<ShadowRibEntry>>>
      trackableObjects_;
  bool verifyInAdjRibPackingList(
      const std::shared_ptr<AdjRib>& adjRib,
      const std::shared_ptr<const BgpPath>& attrs,
      const folly::CIDRNetwork& prefix,
      uint32_t pathId = kPlaceholderPathID) {
    for (const auto& [key, prefixes] : adjRib->attrToPrefixMap_) {
      if (BgpPathCompareWithNull{}(key.attrs, attrs) &&
          prefixes.contains({prefix, pathId})) {
        return true;
      }
    }
    return false;
  }

  nettools::bgplib::MonitoredBackPressuredQueue<RibInMessage> ribInQ_{
      nettools::bgplib::kMaxIngressQueueSize};
  MonitoredMPMCQueue<AdjRib::ObservableMessageT> observerQ_;
};

/*
 * tryAcceptPeerToGroup() tests
 */

/*
 * DSP peer in DETACHED_READY_TO_JOIN with matching RIB-OUT entries.
 * tryAcceptPeerToGroup succeeds: sync bitmap set, removed from detachedPeer
 * set, detached RIB version reset, state transitions to JOINED_RUNNING.
 */
TEST_F(UpdateGroupDetachLifecycleTest, AcceptPeerSucceedsWithMatchingEntries) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib1, 1);

  // Peer 0 is detached DSP (no sync bit set)
  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(42);
  setUpReadyPeerConsumer(adjRib0);

  // Add matching entries in group's LiteTree for both peer and group
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  for (const auto& prefix : {kV4Prefix1, kV4Prefix2, kV4Prefix3}) {
    group_
        ->addToLiteTree(
            group_->LiteTree_, prefix, groupOwnerKey, kPlaceholderPathID)
        ->setPostAttr(attrs);
    group_
        ->addToLiteTree(
            group_->LiteTree_, prefix, peerOwnerKey, kPlaceholderPathID)
        ->setPostAttr(attrs);
  }

  setGroupConsumerReady();
  group_->checkAndAcceptReadyToJoinPeers();

  // Peer should be JOINED_RUNNING
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);

  // Sync bitmap set, removed from detachedPeer set
  EXPECT_TRUE(group_->isPeerInSync(0));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));

  // Detached RIB version reset
  EXPECT_EQ(adjRib0->getDetachedRibVersion(), 0);

  // Peer entries should be collapsed (erased)
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2, kV4Prefix3}) {
    EXPECT_EQ(
        group_->getFromLiteTree(group_->LiteTree_, prefix, peerOwnerKey),
        nullptr);
    EXPECT_NE(
        group_->getFromLiteTree(group_->LiteTree_, prefix, groupOwnerKey),
        nullptr);
  }

  // No PL corrections needed (all entries matched)
  EXPECT_TRUE(adjRib0->attrToPrefixMap_.empty());

  // Peer 1 unaffected
  EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::JOINED_RUNNING);
}

/*
 * DSP peer in DETACHED_READY_TO_JOIN with mismatching RIB-OUT entries.
 * tryAcceptPeerToGroup fails: peer set back to DETACHED_RUNNING,
 * no state changes applied. Packing timers are rescheduled so the peer
 * can continue processing pending changes and attempt to rejoin later.
 */
TEST_F(UpdateGroupDetachLifecycleTest, AcceptPeerFailsWithMismatchingEntries) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib1, 1);

  // Peer 0 is detached (no sync bit set)
  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(42);

  // Set up CL consumer and activate it so the peer has a timer to reschedule
  auto peerConsumer = std::make_shared<AdjRibOutConsumer>(
      changeTracker_,
      adjRib0,
      "peer0_consumer",
      *evb_,
      addPathBitmap_,
      nonAddPathBitmap_);
  adjRib0->setChangeListConsumer(peerConsumer);
  adjRib0->activateChangeListConsumer();
  // Cancel timer so we can verify it gets rescheduled after failed rejoin
  adjRib0->cancelPackingTimers();
  EXPECT_FALSE(adjRib0->changeListConsumeTimer_->isScheduled());

  // Add mismatching entries
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  auto groupAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto peerAttrs = std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));

  group_
      ->addToLiteTree(
          group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID)
      ->setPostAttr(groupAttrs);
  group_
      ->addToLiteTree(
          group_->LiteTree_, kV4Prefix1, peerOwnerKey, kPlaceholderPathID)
      ->setPostAttr(peerAttrs);

  auto acceptedPeers = group_->tryAcceptPeersToGroup({adjRib0});
  EXPECT_TRUE(acceptedPeers.empty());

  // Peer should be set back to DETACHED_RUNNING
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_RUNNING);

  // Still in detachedPeer set, not in sync
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_FALSE(group_->isPeerInSync(0));

  // Detached RIB version updated to group's lastSeenRibVersion
  EXPECT_EQ(adjRib0->getDetachedRibVersion(), group_->getLastSeenRibVersion());

  // Peer entry collapsed (erased), discrepancy corrected via packing list
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);

  // Packing list contains re-advertisement with group's attrs for the
  // discrepant prefix
  EXPECT_TRUE(verifyInAdjRibPackingList(adjRib0, groupAttrs, kV4Prefix1));

  // Packing timers rescheduled so peer can continue processing
  EXPECT_TRUE(adjRib0->changeListConsumeTimer_->isScheduled());
}

/*
 * checkAndAcceptReadyToJoinPeers only processes DETACHED_READY_TO_JOIN peers.
 * Peers in other states are not touched.
 */
TEST_F(
    UpdateGroupDetachLifecycleTest,
    CheckAndAcceptOnlyProcessesReadyToJoinPeers) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto adjRib2 = createAndRegisterPeer(2);
  setUpJoinedRunningPeer(adjRib2, 2);

  // Peer 0: DETACHED_READY_TO_JOIN with matching entries
  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);
  setUpReadyPeerConsumer(adjRib0);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey0 = adjRib0->getPeerOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  group_
      ->addToLiteTree(
          group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID)
      ->setPostAttr(attrs);
  group_
      ->addToLiteTree(
          group_->LiteTree_, kV4Prefix1, peerOwnerKey0, kPlaceholderPathID)
      ->setPostAttr(attrs);

  // Peer 1: DETACHED_RUNNING (should not be processed)
  adjRib1->setPeerState(PeerUpdateState::DETACHED_RUNNING);

  // Peer 2: JOINED_RUNNING (should not be processed)
  // Already set by setUpJoinedRunningPeer

  setGroupConsumerReady();
  group_->checkAndAcceptReadyToJoinPeers();

  // Peer 0 should be accepted
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(0));

  // Peer 1 should remain DETACHED_RUNNING
  EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::DETACHED_RUNNING);

  // Peer 2 should remain JOINED_RUNNING
  EXPECT_EQ(adjRib2->getPeerState(), PeerUpdateState::JOINED_RUNNING);
}

/*
 * Multiple DSP peers in DETACHED_READY_TO_JOIN: one succeeds, one fails.
 * Verify independent handling.
 */
TEST_F(
    UpdateGroupDetachLifecycleTest,
    CheckAndAcceptMultiplePeersMixedResults) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto adjRib2 = createAndRegisterPeer(2);
  setUpJoinedRunningPeer(adjRib2, 2);

  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  auto differentAttrs =
      std::make_shared<BgpPath>(*buildBgpPathFields(2, 2, 0, 0));

  // Peer 0: DETACHED_READY_TO_JOIN with matching entries (will succeed)
  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);
  setUpReadyPeerConsumer(adjRib0);
  auto peerOwnerKey0 = adjRib0->getPeerOwnerKey();
  group_
      ->addToLiteTree(
          group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID)
      ->setPostAttr(attrs);
  group_
      ->addToLiteTree(
          group_->LiteTree_, kV4Prefix1, peerOwnerKey0, kPlaceholderPathID)
      ->setPostAttr(attrs);

  // Peer 1: DETACHED_READY_TO_JOIN with mismatching entries (will fail)
  adjRib1->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib1);
  setUpReadyPeerConsumer(adjRib1);
  auto peerOwnerKey1 = adjRib1->getPeerOwnerKey();
  group_
      ->addToLiteTree(
          group_->LiteTree_, kV4Prefix1, peerOwnerKey1, kPlaceholderPathID)
      ->setPostAttr(differentAttrs);

  setGroupConsumerReady();
  group_->checkAndAcceptReadyToJoinPeers();

  // Peer 0 should be accepted
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(0));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));

  // Peer 1 should be set back to DETACHED_RUNNING
  EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::DETACHED_RUNNING);
  EXPECT_FALSE(group_->isPeerInSync(1));

  // Peer 2 unaffected
  EXPECT_EQ(adjRib2->getPeerState(), PeerUpdateState::JOINED_RUNNING);
}

/*
 * DFP with group-only entries in the RIB-OUT (no peer entries).
 * All entries have ribVersion <= detachedRibVersion since the peer was
 * sharing them before detachment. Collapse should succeed with 0 entries
 * collapsed (nothing to erase).
 */
TEST_F(UpdateGroupDetachLifecycleTest, AcceptDFPWithSharedEntriesOnly) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib1, 1);

  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  adjRib0->setAdjRibFlag(AdjRib::IS_DETACHED_FAST_PEER);
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(10);

  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  // All entries are group-only, announced before detachment
  for (const auto& [prefix, version] :
       std::vector<std::pair<folly::CIDRNetwork, uint64_t>>{
           {kV4Prefix1, 3}, {kV4Prefix2, 7}, {kV4Prefix3, 10}}) {
    auto* entry = group_->addToLiteTree(
        group_->LiteTree_, prefix, groupOwnerKey, kPlaceholderPathID);
    entry->setPostAttr(attrs);
    entry->setRibVersion(version);
  }

  group_->checkAndAcceptReadyToJoinPeers();

  // Peer promoted to JOINED_RUNNING (DFP: no collapse needed)
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_FALSE(adjRib0->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER));
  EXPECT_TRUE(group_->isPeerInSync(0));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_EQ(adjRib0->getDetachedRibVersion(), 0);

  // All group entries still present (DFP doesn't touch them)
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2, kV4Prefix3}) {
    EXPECT_NE(
        group_->getFromLiteTree(group_->LiteTree_, prefix, groupOwnerKey),
        nullptr);
  }
}

/*
 * DSP with a mix of shared (pre-detachment) and lazily-cloned entries.
 * Shared entries: group-only, ribVersion <= detachedRibVersion.
 * Cloned entries: both peer and group, matching attrs.
 * Collapse should succeed — shared entries pass version check, cloned entries
 * are collapsed.
 */
TEST_F(
    UpdateGroupDetachLifecycleTest,
    AcceptDSPWithMixedSharedAndClonedEntries) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib1, 1);

  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(10);
  setUpReadyPeerConsumer(adjRib0);

  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  // Prefix 1 & 2: announced before detachment (shared, group-only)
  auto* entry1 = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  entry1->setPostAttr(attrs);
  entry1->setRibVersion(5);

  auto* entry2 = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix2, groupOwnerKey, kPlaceholderPathID);
  entry2->setPostAttr(attrs);
  entry2->setRibVersion(10);

  // Prefix 3: announced after detachment, lazily cloned to peer
  auto* groupEntry3 = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix3, groupOwnerKey, kPlaceholderPathID);
  groupEntry3->setPostAttr(attrs);
  groupEntry3->setRibVersion(15);

  auto* peerEntry3 = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix3, peerOwnerKey, kPlaceholderPathID);
  peerEntry3->setPostAttr(attrs);
  peerEntry3->setRibVersion(15);

  setGroupConsumerReady();
  group_->checkAndAcceptReadyToJoinPeers();

  // Peer promoted to JOINED_RUNNING
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(0));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_EQ(adjRib0->getDetachedRibVersion(), 0);

  // Shared entries (prefix 1 & 2) — group entry remains, no peer entry
  EXPECT_NE(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, groupOwnerKey),
      nullptr);
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix1, peerOwnerKey),
      nullptr);
  EXPECT_NE(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix2, groupOwnerKey),
      nullptr);
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix2, peerOwnerKey),
      nullptr);

  // Cloned entry (prefix 3) — peer entry collapsed, group entry remains
  EXPECT_NE(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix3, groupOwnerKey),
      nullptr);
  EXPECT_EQ(
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix3, peerOwnerKey),
      nullptr);

  // No PL corrections needed (shared entries pass version check, cloned match)
  EXPECT_TRUE(adjRib0->attrToPrefixMap_.empty());
}

/*
 * DSP with a post-detachment group entry that was NOT lazily cloned.
 * Entry has ribVersion > detachedRibVersion and no peer entry — discrepancy.
 * Collapse should fail, peer set back to DETACHED_RUNNING.
 */
TEST_F(
    UpdateGroupDetachLifecycleTest,
    AcceptDSPFailsWhenPostDetachEntryNotCloned) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib1, 1);

  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);
  adjRib0->setDetachedRibVersion(10);

  // Set up CL consumer so packing timers can be rescheduled on failure
  auto peerConsumer = std::make_shared<AdjRibOutConsumer>(
      changeTracker_,
      adjRib0,
      "peer0_consumer",
      *evb_,
      addPathBitmap_,
      nonAddPathBitmap_);
  adjRib0->setChangeListConsumer(peerConsumer);
  adjRib0->activateChangeListConsumer();
  adjRib0->cancelPackingTimers();

  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  // Prefix 1: announced before detachment — shared, OK
  auto* entry1 = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  entry1->setPostAttr(attrs);
  entry1->setRibVersion(5);

  // Prefix 2: announced after detachment, NOT cloned — discrepancy
  auto* entry2 = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix2, groupOwnerKey, kPlaceholderPathID);
  entry2->setPostAttr(attrs);
  entry2->setRibVersion(15);

  auto acceptedPeers = group_->tryAcceptPeersToGroup({adjRib0});
  EXPECT_TRUE(acceptedPeers.empty());

  // Peer set back to DETACHED_RUNNING
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_RUNNING);
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_FALSE(group_->isPeerInSync(0));

  // Detached RIB version updated to group's lastSeenRibVersion
  EXPECT_EQ(adjRib0->getDetachedRibVersion(), group_->getLastSeenRibVersion());

  // Packing list contains announcement correction for the missing prefix
  EXPECT_TRUE(verifyInAdjRibPackingList(adjRib0, attrs, kV4Prefix2));

  // Packing timers rescheduled
  EXPECT_TRUE(adjRib0->changeListConsumeTimer_->isScheduled());
}

/*
 * Peer detaches as slow peer. Consumer has pending CL items to process
 * → peer is DSP → starts timer-driven CL consumption loop.
 */
TEST_F(UpdateGroupDetachLifecycleTest, DSPDetachAndStartConsumption) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);

  // Publish a CL item but DON'T consume it — group consumer stays pended.
  // When detached consumer joins, it also starts pended (not ready = DSP).
  publishChangeItem(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});

  // Step 1: Peer 0 blocks and detaches
  group_->markPeerBlocked(adjRib0);
  EXPECT_EQ(adjRib0->getPeerBlockInfo().blockCount, 1);
  group_->detachSlowPeer(adjRib0);
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);

  // Step 2: Peer 0 unblocks — DSP path (consumer not ready)
  group_->markPeerUnblocked(adjRib0);

  // Block info is NOT reset on activation — it's reset on rejoin instead.
  EXPECT_EQ(adjRib0->getPeerBlockInfo().blockCount, 1);

  // Pump the evb so sendBgpUpdates runs
  evb_->loopOnce();

  // Peer should NOT transition to DETACHED_READY_TO_JOIN yet
  EXPECT_NE(adjRib0->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);

  // Clean up
  adjRib0->deactivateDetachedModeProcessing();
}

/**
 * Two peers join group. Peer 0 blocks, gets detected as slow, detaches.
 * Group hasn't moved on CL -> peer is DFP -> immediate
 * DETACHED_READY_TO_JOIN with IS_DETACHED_FAST_PEER flag.
 */
TEST_F(UpdateGroupDetachLifecycleTest, DFPDetachAndReadyToJoin) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);

  // Publish a CL item and have the group consumer consume it.
  // After consumption, group consumer is "ready" (at end of CL).
  // DFP scenario: group hasn't moved further on CL after this point.
  publishChangeItem(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});
  groupConsumer_->iterateChanges();
  EXPECT_TRUE(groupConsumer_->isReady());

  // Add a prefix to the group packing list (group is mid-send)
  addPrefixToGroupPL(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});

  // Step 1: Peer 0 becomes blocked
  group_->markPeerBlocked(adjRib0);
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_BLOCKED);
  EXPECT_TRUE(BitmapUtils::isBitSet(group_->getBlockedBitmap(), 0));

  // Step 2: Slow peer detection fires, peer detaches
  group_->detachSlowPeer(adjRib0);
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_FALSE(group_->isPeerInSync(0));
  EXPECT_FALSE(BitmapUtils::isBitSet(group_->getBlockedBitmap(), 0));

  // Verify detached consumer was created
  EXPECT_NE(adjRib0->getDetachedConsumer(), nullptr);

  // DFP: consumer is ready (joined at group's ready position), PL empty
  adjRib0->clearPackingList();

  // Step 3: Peer 0 unblocks
  group_->markPeerUnblocked(adjRib0);

  // Pump the evb so sendBgpUpdates runs and DFP check triggers
  evb_->loopOnce();

  // DFP: isDFP() returns true (group PL non-empty, versions match)
  // -> transition to DETACHED_READY_TO_JOIN with IS_DETACHED_FAST_PEER flag
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);
  EXPECT_TRUE(adjRib0->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER));

  // Peer 1 should be completely unaffected
  EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(1));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib1));

  // Clean up
  adjRib0->deactivateDetachedModeProcessing();
}

/*
 * Two peers block. Both get detected as slow and detach.
 * Verify independent detachment and processing.
 */
TEST_F(UpdateGroupDetachLifecycleTest, TwoPeersDetachIndependently) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  auto adjRib2 = createAndRegisterPeer(2);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);
  setUpJoinedRunningPeer(adjRib2, 2);

  // Publish a CL item and consume it so group consumer is ready
  publishChangeItem(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});
  groupConsumer_->iterateChanges();

  // Add prefix to group PL
  addPrefixToGroupPL(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});

  // Both peer 0 and 1 block
  group_->markPeerBlocked(adjRib0);
  group_->markPeerBlocked(adjRib1);

  // Both detach
  group_->detachSlowPeer(adjRib0);
  group_->detachSlowPeer(adjRib1);

  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
  EXPECT_EQ(adjRib1->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib1));

  // Peer 2 is unaffected
  EXPECT_EQ(adjRib2->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(2));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib2));

  // Peer 0: DFP — consumer is ready (joined at group's ready position), PL
  // empty
  adjRib0->clearPackingList();

  // Unblock peer 0 — DFP path
  group_->markPeerUnblocked(adjRib0);
  evb_->loopOnce();

  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);
  EXPECT_TRUE(adjRib0->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER));

  // Publish a new CL item — pends peer 1's consumer so it's no longer ready
  publishChangeItem(folly::CIDRNetwork{folly::IPAddress("20.0.0.0"), 24});

  // Advance group's version so peer 1 fails the DFP version check
  group_->setLastSeenRibVersion(group_->getLastSeenRibVersion() + 1);

  // Clear peer 1's PL — tests don't have boundedAdjRibOutQueue_ for sending
  adjRib1->clearPackingList();

  // Unblock peer 1 — DSP path (consumer not ready, has pending CL item)
  group_->markPeerUnblocked(adjRib1);
  evb_->loopOnce();

  // Peer 1 should NOT transition to DETACHED_READY_TO_JOIN yet
  EXPECT_NE(adjRib1->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);

  // Clean up
  adjRib0->deactivateDetachedModeProcessing();
  adjRib1->deactivateDetachedModeProcessing();
}

/*
 * Peer detaches, reaches DETACHED_READY_TO_JOIN, then rejoins via
 * checkAndAcceptReadyToJoinPeers. Block info should be reset on rejoin,
 * not on detach activation.
 */
TEST_F(UpdateGroupDetachLifecycleTest, BlockCountResetOnRejoin) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);

  // Publish a CL item and consume it so group consumer is ready
  publishChangeItem(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});
  groupConsumer_->iterateChanges();
  EXPECT_TRUE(groupConsumer_->isReady());

  // Add a prefix to the group packing list (group is mid-send)
  addPrefixToGroupPL(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});

  // Step 1: Peer 0 blocks and detaches
  group_->markPeerBlocked(adjRib0);
  EXPECT_EQ(adjRib0->getPeerBlockInfo().blockCount, 1);
  group_->detachSlowPeer(adjRib0);
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);

  // DFP: consumer is ready, PL empty
  adjRib0->clearPackingList();

  // Step 2: Peer 0 unblocks — DFP path
  group_->markPeerUnblocked(adjRib0);

  // Block count should NOT be reset on activation
  EXPECT_EQ(adjRib0->getPeerBlockInfo().blockCount, 1);

  // Pump the evb so sendBgpUpdates runs and DFP check triggers
  evb_->loopOnce();

  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_READY_TO_JOIN);
  EXPECT_TRUE(adjRib0->isAdjRibFlagSet(AdjRib::IS_DETACHED_FAST_PEER));

  // Block count still preserved before rejoin
  EXPECT_EQ(adjRib0->getPeerBlockInfo().blockCount, 1);

  // Step 3: Rejoin — checkAndAcceptReadyToJoinPeers resets block info
  group_->checkAndAcceptReadyToJoinPeers();

  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_TRUE(group_->isPeerInSync(0));

  // Block count should be reset on rejoin
  EXPECT_EQ(adjRib0->getPeerBlockInfo().blockCount, 0);
}

/*
 * DSP peer in DETACHED_READY_TO_JOIN when group consumer is NOT at end of CL.
 * Peer cannot rejoin — should transition back to DETACHED_RUNNING and
 * reschedule packing timers.
 */
TEST_F(UpdateGroupDetachLifecycleTest, CheckAndAcceptDSPPeerGroupNotReady) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib1, 1);

  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);

  // Set up CL consumer and activate so the peer has timers
  auto peerConsumer = std::make_shared<AdjRibOutConsumer>(
      changeTracker_,
      adjRib0,
      "peer0_consumer",
      *evb_,
      addPathBitmap_,
      nonAddPathBitmap_);
  adjRib0->setChangeListConsumer(peerConsumer);
  adjRib0->activateChangeListConsumer();
  adjRib0->cancelPackingTimers();
  EXPECT_FALSE(adjRib0->changeListConsumeTimer_->isScheduled());

  // Set peer consumer to ready
  setUpReadyPeerConsumer(adjRib0);

  // Do NOT call setGroupConsumerReady() — group consumer marker is nullptr
  // (freshly registered), so isReady() returns true and the peer can rejoin.
  group_->checkAndAcceptReadyToJoinPeers();

  // Peer rejoins — group never published any changes, so both consumers
  // are at nullptr (ready)
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_TRUE(group_->isPeerInSync(0));
}

/*
 * DSP peer in DETACHED_READY_TO_JOIN when group consumer IS at end of CL
 * and peer's consumer has a null marker (no changes published). Peer can
 * rejoin since both consumers are ready.
 */
TEST_F(UpdateGroupDetachLifecycleTest, CheckAndAcceptDSPPeerConsumerNotReady) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib1, 1);

  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib0);

  // Set up CL consumer and activate so the peer has timers
  auto peerConsumer = std::make_shared<AdjRibOutConsumer>(
      changeTracker_,
      adjRib0,
      "peer0_consumer",
      *evb_,
      addPathBitmap_,
      nonAddPathBitmap_);
  adjRib0->setChangeListConsumer(peerConsumer);
  adjRib0->activateChangeListConsumer();
  adjRib0->cancelPackingTimers();
  EXPECT_FALSE(adjRib0->changeListConsumeTimer_->isScheduled());

  // Peer consumer marker is nullptr (no changes published), so isReady()
  // returns true.
  EXPECT_TRUE(adjRib0->isReadyToRejoinGroup());

  // Group consumer IS ready
  setGroupConsumerReady();

  group_->checkAndAcceptReadyToJoinPeers();

  // Peer rejoins — both consumers are at nullptr (ready)
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_TRUE(group_->isPeerInSync(0));
}

/**
 * Test: getRibEntryWithUpdateGroup returns per-peer entry when it exists
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    GetRibEntryWithUpdateGroupReturnsPeerEntry) {
  auto adjRib = createAndRegisterPeer(0);

  folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};
  auto peerOwnerKey = adjRib->getPeerOwnerKey();

  // Add a per-peer entry directly to the group's tree
  auto added =
      group_->addToLiteTree(group_->LiteTree_, prefix, peerOwnerKey, 0);
  ASSERT_NE(added, nullptr);

  auto* entry = adjRib->getRibEntryWithUpdateGroup(prefix, 0);
  EXPECT_EQ(entry, added);
}

/**
 * Test: getRibEntryWithUpdateGroup clones shared group entry to per-peer entry
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    GetRibEntryWithUpdateGroupCopiesSharedEntry) {
  auto adjRib = createAndRegisterPeer(0);
  // Register a second peer so copyEntryForPeer uses peer owner key.
  auto adjRib1 = createAndRegisterPeer(1);
  adjRib->setDetachedRibVersion(10);

  folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};
  auto groupOwnerKey = AdjRibOutOwnerKey::forGroup(1);

  // Add a group entry with ribVersion=5 (< detachedRibVersion=10)
  auto groupEntry =
      group_->addToLiteTree(group_->LiteTree_, prefix, groupOwnerKey, 0);
  ASSERT_NE(groupEntry, nullptr);
  groupEntry->setRibVersion(5);

  auto entry = adjRib->getRibEntryWithUpdateGroup(prefix, 0);

  // Should return a cloned per-peer entry, not the group entry
  EXPECT_NE(entry, nullptr);
  EXPECT_NE(entry, groupEntry);
  // Cloned entry should have the same ribVersion
  EXPECT_EQ(entry->getRibVersion(), groupEntry->getRibVersion());

  // Verify group entry is still in the tree untouched
  auto groupEntryAfter =
      group_->getFromLiteTree(group_->LiteTree_, prefix, groupOwnerKey);
  EXPECT_EQ(groupEntryAfter, groupEntry);
}

/**
 * Test: getRibEntryWithUpdateGroup returns nullptr when no entry exists
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    GetRibEntryWithUpdateGroupReturnsNullWhenNoEntry) {
  auto adjRib = createAndRegisterPeer(0);

  folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};

  auto* entry = adjRib->getRibEntryWithUpdateGroup(prefix, 0);
  EXPECT_EQ(entry, nullptr);
}

/**
 * Test: getRibEntryWithUpdateGroup skips group entry when peer never saw it
 * (detachedRibVersion < groupEntry ribVersion)
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    GetRibEntryWithUpdateGroupSkipsUnseenGroupEntry) {
  auto adjRib = createAndRegisterPeer(0);
  adjRib->setDetachedRibVersion(5);

  folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};
  auto groupOwnerKey = AdjRibOutOwnerKey::forGroup(1);

  // Add a group entry with ribVersion=10 (> detachedRibVersion=5)
  auto groupEntry =
      group_->addToLiteTree(group_->LiteTree_, prefix, groupOwnerKey, 0);
  ASSERT_NE(groupEntry, nullptr);
  groupEntry->setRibVersion(10);

  auto* entry = adjRib->getRibEntryWithUpdateGroup(prefix, 0);
  EXPECT_EQ(entry, nullptr);

  // Verify group entry is still in the tree untouched
  auto groupEntryAfter =
      group_->getFromLiteTree(group_->LiteTree_, prefix, groupOwnerKey);
  EXPECT_EQ(groupEntryAfter, groupEntry);
}

TEST_F(UpdateGroupDetachedPeerTest, DetachSlowPeerSetsEgressEoRWhenFlagIsTrue) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);

  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;
  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();
  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());
  ASSERT_NE(groupConsumer->getMarker(), nullptr);

  adjRib0->setPeerState(PeerUpdateState::JOINED_BLOCKED);
  adjRib1->setPeerState(PeerUpdateState::JOINED_RUNNING);
  group_->setSyncBitForTesting(0);
  group_->setSyncBitForTesting(1);
  group_->markPeerBlocked(adjRib0);

  EXPECT_FALSE(adjRib0->egressEoRsPending_);

  group_->detachSlowPeer(adjRib0, true /* sendWithEoR */);

  /* Peer should have egressEoRsPending set via sendWithEoR */
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
  EXPECT_TRUE(adjRib0->egressEoRsPending_);

  /* Clean up consumers before group destruction */
  adjRib0->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
  groupConsumer.reset();
}

/**
 * Test: detachSlowPeer does not set egressEoRsPending when group doesn't
 * have it pending
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    DetachDoesNotSetEoRsPendingWhenGroupHasNone) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);

  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  ShadowRibEntry entry;
  entry.prefix = folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(entry));
  changeTracker->publishChange(trackable.get());

  adjRib0->setPeerState(PeerUpdateState::JOINED_BLOCKED);
  adjRib1->setPeerState(PeerUpdateState::JOINED_RUNNING);
  group_->setSyncBitForTesting(0);
  group_->setSyncBitForTesting(1);
  group_->markPeerBlocked(adjRib0);

  EXPECT_FALSE(adjRib0->egressEoRsPending_);

  group_->detachSlowPeer(adjRib0);

  /* Peer should still not have egressEoRsPending (sendWithEoR defaults false)
   */
  EXPECT_FALSE(adjRib0->egressEoRsPending_);

  /* Clean up consumers before group destruction */
  adjRib0->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
  groupConsumer.reset();
}

/**
 * Test: Initial RIB dump with sendWithEoR=true threads the flag through
 * the distribution path. When a slow peer gets detached during distribution,
 * sendWithEoR is used to set egressEoRsPending_ on the detached peer.
 *
 * Flow:
 *   1. walkAndProcessShadowRib(true) populates packing list
 *   2. processRibOutAnnouncement sets egressEoRsPending_ = true
 *   3. buildAndSendGroupBgpMessages(sendWithEoR=true) distributes messages
 *   4. distributeMessageToInSyncPeers finds peer0's queue blocked
 *   5. markPeerBlocked triggers detachSlowPeer(adjRib, sendWithEoR=true)
 *   6. detachSlowPeer sets egressEoRsPending_ on peer via sendWithEoR
 */
TEST_F(UpdateGroupDetachedPeerTest, RibWalkDetachCopiesEgressEoRsPending) {
  /* Build shadow RIB with one entry */
  ShadowRibEntriesMap shadowRibEntries;
  folly::CIDRNetwork prefix{folly::IPAddress("10.0.0.0"), 24};

  ShadowRibEntry srEntry;
  srEntry.prefix = prefix;
  srEntry.ribVersion = 1;
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  attrs->setLocalPref(100);
  TinyPeerInfo peerInfo{
      folly::IPAddress("1.1.1.1"), 65000, 0, BgpSessionType::EBGP, false};
  srEntry.bestpath =
      std::make_shared<ShadowRibRouteInfo>(peerInfo, attrs, kDefaultPathID);

  shadowRibEntries.emplace(
      prefix,
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(srEntry)));

  /* Create group with the shadow RIB (IPv4 negotiated for the entry to pass) */
  UpdateGroupKey groupKey;
  groupKey.afiIpv4Negotiated = true;
  UpdateGroupConfig ugConfig;
  ugConfig.allowSlowPeerDetach = true;
  group_ = std::make_shared<AdjRibOutGroup>(
      *evb_,
      "test_group",
      1,
      true /* enableUpdateGroup */,
      groupKey,
      &shadowRibEntries,
      nullptr /* policyManager */,
      ugConfig);

  /* Set slow peer threshold to 1 so first block triggers detach */
  {
    UpdateGroupConfig cfg;
    cfg.allowSlowPeerDetach = true;
    cfg.slowPeerTimeThreshold = std::chrono::milliseconds(50000);
    cfg.slowPeerBlockCountThreshold = 1;
    cfg.slowPeerBlockCountWindow = std::chrono::milliseconds(300000);
    group_->setUpdateGroupConfigForTesting(cfg);
  }

  /* Register two peers */
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);

  /* Give peer0 a tiny bounded queue that's already blocked */
  auto blockedQueue = std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(3, 2, 1);
  /* Fill past high watermark to make it blocked */
  blockedQueue->push(std::nullopt);
  blockedQueue->push(std::nullopt);
  ASSERT_TRUE(blockedQueue->isBlocked());
  adjRib0->boundedAdjRibOutQueue_ = blockedQueue;

  /* Give peer1 a normal queue */
  adjRib1->boundedAdjRibOutQueue_ =
      std::make_shared<AdjRib::BoundedAdjRibOutQueueT>(1000, 500, 100);

  /* Set up change tracker and consumer for detachSlowPeer */
  auto changeTracker =
      std::make_shared<ChangeTracker<ShadowRibEntry>>("test_tracker");
  ConsumerBitmap addPathBitmap;
  ConsumerBitmap nonAddPathBitmap;

  auto groupConsumer = std::make_shared<AdjRibOutGroupConsumer>(
      changeTracker,
      group_,
      "test_group_consumer",
      *evb_,
      addPathBitmap,
      nonAddPathBitmap);
  groupConsumer->registerWithTracker();
  groupConsumer->setBitmap();

  group_->setChangeListConsumer(groupConsumer);
  group_->setChangeListTracker(changeTracker, addPathBitmap, nonAddPathBitmap);

  /* Publish an item so the consumer has a non-null marker */
  ShadowRibEntry clEntry;
  clEntry.prefix = folly::CIDRNetwork{folly::IPAddress("20.0.0.0"), 24};
  auto trackable =
      std::make_unique<TrackableObject<ShadowRibEntry>>(std::move(clEntry));
  changeTracker->publishChange(trackable.get());

  /* Set peer states and sync bits */
  adjRib0->setPeerState(PeerUpdateState::JOINED_RUNNING);
  adjRib1->setPeerState(PeerUpdateState::JOINED_RUNNING);
  group_->setSyncBitForTesting(0);
  group_->setSyncBitForTesting(1);

  /* Peer should not have egressEoRsPending before rib walk */
  EXPECT_FALSE(adjRib0->egressEoRsPending_);

  /* Walk shadow RIB with sendWithEoR=true (initial dump) */
  group_->walkAndProcessShadowRib(true /* sendWithEoR */);

  /*
   * Pump the event loop to let buildAndSendGroupBgpMessages run.
   * sendWithEoR=true is threaded through the distribution path.
   * This will:
   *   1. Try to distribute messages
   *   2. peer0's queue is blocked -> markPeerBlocked -> detachSlowPeer
   *   3. detachSlowPeer sets egressEoRsPending_ on peer via sendWithEoR
   */
  evb_->loopOnce();

  /* Peer0 should have been detached and inherited egressEoRsPending_ */
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_TRUE(adjRib0->egressEoRsPending_);

  /*
   * Group's egressEoRsPending_ should be reset after
   * buildAndSendGroupBgpMessages
   */
  EXPECT_FALSE(group_->egressEoRsPending_);

  /* Close blocked queue and pump evb so deferredPushToPeer can exit */
  blockedQueue->close();
  evb_->loopOnce();

  /* Clean up */
  adjRib0->deactivateDetachedModeProcessing();
  groupConsumer->resetBitmap();
  groupConsumer->terminate();
  groupConsumer->deregisterFromTracker();
  group_->resetChangeListConsumer();
  groupConsumer.reset();
}

/*
 * Test: collapseLiteEntry skips detachedRibVersion check for init dump peers
 * (WAS_DETACHED_INIT_DUMP_PEER flag) and queues announcements for all
 * group-only entries.
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    CollapseLiteSkipsRibVersionCheckForInitDumpPeer) {
  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  /* Add group-only entry with ribVersion=5, lower than detachedRibVersion */
  auto* groupEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix1, groupOwnerKey, kPlaceholderPathID);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(5);

  /* Set detachedRibVersion=10 — normally ribVersion(5) <= 10 would skip */
  adjRib->setDetachedRibVersion(10);
  adjRib->setAdjRibFlag(AdjRib::WAS_DETACHED_INIT_DUMP_PEER);
  adjRib->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib);

  group_->collapseLiteEntries(groupOwnerKey, {adjRib});

  /* Despite ribVersion < detachedRibVersion, init dump peer
   * should still get the announcement queued */
  EXPECT_TRUE(adjRib->isAdjRibFlagSet(AdjRib::RIB_OUT_DISCREPANCY));
  EXPECT_FALSE(adjRib->attrToPrefixMap_.empty());
}

/**
 * Test: collapsePathEntry skips detachedRibVersion check for init dump peers
 * (WAS_DETACHED_INIT_DUMP_PEER flag) and queues announcements for all
 * group-only entries.
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    CollapsePathSkipsRibVersionCheckForInitDumpPeer) {
  /* Create group with sendAddPath=true for path tree */
  UpdateGroupKey groupKey;
  groupKey.sendAddPath = true;
  group_ = std::make_shared<AdjRibOutGroup>(
      *evb_, "test_group", 1, true /* enableUpdateGroup */, groupKey);

  auto adjRib = createAndRegisterPeer(0);
  auto groupOwnerKey = group_->getGroupOwnerKey();

  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));

  /* Add group-only entry with ribVersion=5, lower than detachedRibVersion */
  auto* groupEntry = group_->addToPathTree(
      group_->PathTree_, kV4Prefix1, groupOwnerKey, 42 /* pathId */);
  groupEntry->setPostAttr(attrs);
  groupEntry->setRibVersion(5);

  /* Set detachedRibVersion=10 — normally ribVersion(5) <= 10 would skip */
  adjRib->setDetachedRibVersion(10);
  adjRib->setAdjRibFlag(AdjRib::WAS_DETACHED_INIT_DUMP_PEER);
  adjRib->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  group_->markPeerDetached(adjRib);

  group_->collapsePathEntries(groupOwnerKey, {adjRib});

  /* Despite ribVersion < detachedRibVersion, init dump peer
   * should still get the announcement queued */
  EXPECT_TRUE(adjRib->isAdjRibFlagSet(AdjRib::RIB_OUT_DISCREPANCY));
  EXPECT_FALSE(adjRib->attrToPrefixMap_.empty());
}

/**
 * Test: When update group is enabled, peer's canAnnounce delegates to
 * canAnnounceForGroup. An IBGP non-RR peer that would normally block
 * IBGP-learned routes allows them when the group has isRrClient=true.
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    CanAnnounceDelegatesToGroupWhenUpdateGroupEnabled) {
  /* Create group with isRrClient=true */
  UpdateGroupKey groupKey;
  groupKey.sessionType = BgpSessionType::IBGP;
  groupKey.isRrClient = true;
  group_ = std::make_shared<AdjRibOutGroup>(
      *evb_, "test_group", 1, true /* enableUpdateGroup */, groupKey);

  auto adjRib = createAndRegisterPeer(0);

  /* Build an IBGP-learned route from a non-RR client peer */
  TinyPeerInfo peerInfo{
      folly::IPAddress("2.2.2.2"),
      65000,
      0,
      BgpSessionType::IBGP,
      false /* isRrClient */};
  RibOutAnnouncementEntry entry{
      folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24},
      kDefaultPathID,
      peerInfo,
      std::make_shared<BgpPath>(BgpPathFields())};

  /*
   * Peer's own peeringParams_.isRrClient is false (default PeeringParams),
   * so without update group, this would be blocked. With update group,
   * it delegates to canAnnounceForGroup which uses groupKey.isRrClient=true.
   */
  EXPECT_TRUE(adjRib->canAnnounceEntry(entry));
}

/**
 * Test: When update group is enabled with isRrClient=false (non-RR IBGP group),
 * canAnnounce blocks IBGP-learned routes from non-RR peers, even if the
 * peer itself has isRrClient=true.
 */
TEST_F(UpdateGroupDetachedPeerTest, CanAnnounceUsesGroupRrClientSetting) {
  /* Create group with isRrClient=false */
  UpdateGroupKey groupKey;
  groupKey.sessionType = BgpSessionType::IBGP;
  groupKey.isRrClient = false;
  group_ = std::make_shared<AdjRibOutGroup>(
      *evb_, "test_group", 1, true /* enableUpdateGroup */, groupKey);

  auto adjRib = createAndRegisterPeer(0);
  /* Override the peer's RR client setting — should be ignored */
  adjRib->peeringParams_.isRrClient = true;

  /* Build an IBGP-learned route from a non-RR client peer */
  TinyPeerInfo peerInfo{
      folly::IPAddress("2.2.2.2"),
      65000,
      0,
      BgpSessionType::IBGP,
      false /* isRrClient */};
  RibOutAnnouncementEntry entry{
      folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24},
      kDefaultPathID,
      peerInfo,
      std::make_shared<BgpPath>(BgpPathFields())};

  /*
   * Group says isRrClient=false and route is from IBGP non-RR peer,
   * so canAnnounceForGroup returns false.
   */
  EXPECT_FALSE(adjRib->canAnnounceEntry(entry));
}

/*
 * Test: When update group is enabled, canAnnounceEntry skips the
 * sender_suppress_as_loop check. The group-level path does not perform
 * per-peer AS loop suppression because it uses per-peer remoteAs which
 * is incompatible with the shared group processing model.
 *
 * Verifies:
 *  - With enableUpdateGroup_=true: canAnnounceEntry returns true even when
 *    the AS path contains the remote AS (suppress_as_loop is skipped).
 *  - With enableUpdateGroup_=false: canAnnounceEntry returns false for the
 *    same entry (suppress_as_loop kicks in).
 */
TEST_F(
    UpdateGroupDetachedPeerTest,
    CanAnnounceEntrySkipsSuppressAsLoopWithUpdateGroup) {
  /* Create an eBGP group so canAnnounceForGroup returns true */
  UpdateGroupKey groupKey;
  groupKey.sessionType = BgpSessionType::EBGP;
  groupKey.isRrClient = false;
  group_ = std::make_shared<AdjRibOutGroup>(
      *evb_, "test_group", 1, true /* enableUpdateGroup */, groupKey);

  auto adjRib = createAndRegisterPeer(0);

  /* Configure as eBGP peer with a known remote AS.
   * localAs must differ from remoteAs so isIBgpPeer() returns false. */
  constexpr uint32_t remoteAs = 65001;
  adjRib->peeringParams_.remoteAs = remoteAs;
  adjRib->peeringParams_.localAs = 65000;
  adjRib->peeringParams_.isConfedPeer = ConfedPeerConfigured{false};
  adjRib->sender_suppress_as_loop_ = true;

  /* Build a BgpPath whose AS path contains the remote AS (would trigger
   * suppress_as_loop in per-peer mode) */
  auto attrs = std::make_shared<BgpPath>(BgpPathFields());
  nettools::bgplib::BgpAttrAsPathC asPath;
  nettools::bgplib::BgpAttrAsPathSegmentC seg;
  seg.asSequence.push_back(remoteAs);
  seg.asSequence.push_back(65000);
  asPath.push_back(seg);
  attrs->setAsPath(std::move(asPath));

  /* Build announcement entry from a different peer */
  TinyPeerInfo peerInfo{
      folly::IPAddress("3.3.3.3"),
      65002,
      0,
      BgpSessionType::EBGP,
      false /* isRrClient */};
  RibOutAnnouncementEntry entry{
      folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24},
      kDefaultPathID,
      peerInfo,
      attrs};

  /* With update group enabled: should return true (suppress_as_loop skipped) */
  EXPECT_TRUE(adjRib->canAnnounceEntry(entry));

  /* Disable update group: suppress_as_loop should now block the entry */
  adjRib->enableUpdateGroup_ = false;
  EXPECT_FALSE(adjRib->canAnnounceEntry(entry));
}

/*
 * Test: detachSlowPeer copies egress prefix counts from group to peer.
 * After detach, the peer processes new announcements independently (its
 * preOut/postOut counts increase). When the peer rejoins the group via
 * collapse, discrepancies are resolved and the peer's counts match the
 * group's counts.
 *
 * Flow:
 *   1. Two peers JOINED_RUNNING, group announces 3 prefixes
 *   2. Peer 0 detaches — gets group's prefix counts copied
 *   3. Peer 0 independently processes a new prefix (counts diverge)
 *   4. Group withdraws that new prefix while peer 0 is blocked
 *   5. Peer 0 rejoins — collapse resolves the discrepancy
 *   6. After rejoin, peer 0's counts match the group's counts
 */
TEST_F(
    UpdateGroupDetachLifecycleTest,
    DetachCopiesEgressStatsAndCollapseReconciles) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);

  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  TinyPeerInfo peerInfo{
      folly::IPAddress("1.1.1.1"), 65000, 0, BgpSessionType::EBGP, false};

  auto totalSentBefore = totalSentPrefixCount;

  /* Step 1: Group announces 3 v4 prefixes via processRibAnnouncedEntryForGroup
   */
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2, kV4Prefix3}) {
    RibOutAnnouncementEntry entry{prefix, kDefaultPathID, peerInfo, attrs};
    group_->processRibAnnouncedEntryForGroup(entry);
  }

  EXPECT_EQ(group_->getStats().getPostOutPrefixCount(), 3);
  EXPECT_EQ(group_->getStats().getPreOutPrefixCount(), 3);
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 0);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 0);
  /* 2 in-sync peers * 3 prefixes = 6 */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 6);

  /* Step 2: Detach peer 0 — stats should be copied from group.
   * totalSentPrefixCount should not change. */
  auto totalSentAfterStep1 = totalSentPrefixCount;
  adjRib0->setPeerState(PeerUpdateState::JOINED_BLOCKED);
  group_->markPeerBlocked(adjRib0);
  group_->detachSlowPeer(adjRib0);

  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::DETACHED_BLOCKED);
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 3);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 3);
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCountIpv4(), 3);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCountIpv4(), 3);
  EXPECT_EQ(totalSentPrefixCount, totalSentAfterStep1);

  /* Step 3: Peer 0 independently processes a new prefix (kV4Prefix4).
   * This simulates the detached peer consuming from the change list
   * and announcing a prefix the group also has. */
  auto peerEntry = group_->addToLiteTree(
      group_->LiteTree_, kV4Prefix4, peerOwnerKey, kPlaceholderPathID);
  peerEntry->setPostAttr(attrs);
  peerEntry->setRibVersion(100);
  adjRib0->incrementPostOutPrefixCount(true /* isIpv4 */);
  adjRib0->incrementPreOutPrefixCount(true /* isIpv4 */);
  adjRib0->setDetachedRibVersion(50);

  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 4);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 4);

  /* Step 4: Group also processes the same prefix. Group and peer now both
   * have an entry for kV4Prefix4 — no discrepancy for this prefix.
   * Copy the group's postAttr to the peer entry so hasMatchingPostPolicyAttrs
   * matches (it compares shared_ptr identity, not deep content). */
  RibOutAnnouncementEntry entry4(
      kV4Prefix4,
      kDefaultPathID,
      peerInfo,
      attrs,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      false,
      std::chrono::system_clock::now(),
      100 /* ribVersion */);
  group_->processRibAnnouncedEntryForGroup(entry4);
  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto* groupEntry4 =
      group_->getFromLiteTree(group_->LiteTree_, kV4Prefix4, groupOwnerKey);
  ASSERT_NE(groupEntry4, nullptr);
  peerEntry->setPostAttr(groupEntry4->getPostAttr());

  EXPECT_EQ(group_->getStats().getPostOutPrefixCount(), 4);
  EXPECT_EQ(group_->getStats().getPreOutPrefixCount(), 4);
  /* 6 from step 1 + 1 (peer 0 detached increment) + 1 (group with 1 sync peer)
   */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 8);

  /* Step 5: Peer 0 rejoins. Clear PL to simulate that sendBgpUpdates was run
   * and packing list was drained. Collapse should find matching entries and
   * accept the peer. */
  adjRib0->clearPackingList();
  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  setUpReadyPeerConsumer(adjRib0);
  setGroupConsumerReady();
  group_->checkAndAcceptReadyToJoinPeers();

  /* After rejoin, peer's counts should match the group's counts */
  EXPECT_EQ(adjRib0->getPeerState(), PeerUpdateState::JOINED_RUNNING);
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 4);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 4);
  EXPECT_EQ(
      adjRib0->getStats().getPostOutPrefixCount(),
      group_->getStats().getPostOutPrefixCount());
  EXPECT_EQ(
      adjRib0->getStats().getPreOutPrefixCount(),
      group_->getStats().getPreOutPrefixCount());
}

/*
 * Test: After detach, group withdraws a prefix that the peer still has.
 * On rejoin, collapse detects the discrepancy (peer-only entry) and
 * decrements the peer's prefix counts to match the group.
 */
TEST_F(
    UpdateGroupDetachLifecycleTest,
    CollapseDecrementsStatsOnWithdrawalDiscrepancy) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);

  auto groupOwnerKey = group_->getGroupOwnerKey();
  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  TinyPeerInfo peerInfo{
      folly::IPAddress("1.1.1.1"), 65000, 0, BgpSessionType::EBGP, false};

  auto totalSentBefore = totalSentPrefixCount;

  /* Group announces 3 prefixes (2 in-sync peers) */
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2, kV4Prefix3}) {
    RibOutAnnouncementEntry entry{prefix, kDefaultPathID, peerInfo, attrs};
    group_->processRibAnnouncedEntryForGroup(entry);
  }
  /* 2 peers * 3 prefixes = 6 */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 6);

  /* Detach peer 0 — gets group's counts (3 each) */
  adjRib0->setPeerState(PeerUpdateState::JOINED_BLOCKED);
  group_->markPeerBlocked(adjRib0);
  group_->detachSlowPeer(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 3);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 3);
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 6);

  /* Peer clones group entries for the 3 prefixes it was sharing */
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2, kV4Prefix3}) {
    auto peerEntry = group_->addToLiteTree(
        group_->LiteTree_, prefix, peerOwnerKey, kPlaceholderPathID);
    peerEntry->setPostAttr(attrs);
    peerEntry->setRibVersion(10);
  }

  /* Group withdraws kV4Prefix3 while peer 0 is detached.
   * Remove the group's owner entry from the tree node, leaving peer-only. */
  auto itr3 =
      group_->getRadixNodeItrFromLiteTree(group_->LiteTree_, kV4Prefix3);
  ASSERT_NE(itr3, group_->LiteTree_.end());
  itr3->value().erase(groupOwnerKey);
  /* 1 in-sync peer remaining after detach */
  group_->stats_.decrementPostOutPrefixCount(true /* isIpv4 */, 1);
  group_->stats_.decrementPreOutPrefixCount(true /* isIpv4 */);
  /* 6 - 1 (group withdrawal with 1 sync peer) = 5 */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 5);

  EXPECT_EQ(group_->getStats().getPostOutPrefixCount(), 2);
  EXPECT_EQ(group_->getStats().getPreOutPrefixCount(), 2);

  /* Peer 0 still thinks it has 3 prefixes */
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 3);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 3);

  /* Peer 0 rejoins — collapse should detect peer-only entry for kV4Prefix3
   * and decrement peer's counts. Clear PL first so isReadyToRejoinGroup()
   * passes. */
  adjRib0->clearPackingList();
  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  setUpReadyPeerConsumer(adjRib0);
  setGroupConsumerReady();
  group_->checkAndAcceptReadyToJoinPeers();

  /* Peer has discrepancy so it goes back to DETACHED_RUNNING, but
   * the stats should have been adjusted by the collapse logic */
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 2);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 2);
  EXPECT_EQ(
      adjRib0->getStats().getPostOutPrefixCount(),
      group_->getStats().getPostOutPrefixCount());
  EXPECT_EQ(
      adjRib0->getStats().getPreOutPrefixCount(),
      group_->getStats().getPreOutPrefixCount());
  /* 5 - 1 (collapse withdrawal for 1 peer) = 4 */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 4);
}

/*
 * Test: After detach, group announces a new prefix that the peer doesn't have.
 * On rejoin, collapse detects the discrepancy (group-only entry) and
 * increments the peer's prefix counts.
 */
TEST_F(
    UpdateGroupDetachLifecycleTest,
    CollapseIncrementsStatsOnAnnouncementDiscrepancy) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);

  auto peerOwnerKey = adjRib0->getPeerOwnerKey();
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  TinyPeerInfo peerInfo{
      folly::IPAddress("1.1.1.1"), 65000, 0, BgpSessionType::EBGP, false};

  auto totalSentBefore = totalSentPrefixCount;

  /* Group announces 2 prefixes (2 in-sync peers) */
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2}) {
    RibOutAnnouncementEntry entry{prefix, kDefaultPathID, peerInfo, attrs};
    group_->processRibAnnouncedEntryForGroup(entry);
  }
  /* 2 peers * 2 prefixes = 4 */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 4);

  /* Detach peer 0 — gets group's counts (2 each) */
  adjRib0->setPeerState(PeerUpdateState::JOINED_BLOCKED);
  group_->markPeerBlocked(adjRib0);
  group_->detachSlowPeer(adjRib0);
  adjRib0->setDetachedRibVersion(50);

  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 2);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 2);
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 4);

  /* Peer clones group entries for the 2 prefixes it was sharing */
  for (const auto& prefix : {kV4Prefix1, kV4Prefix2}) {
    auto peerEntry = group_->addToLiteTree(
        group_->LiteTree_, prefix, peerOwnerKey, kPlaceholderPathID);
    peerEntry->setPostAttr(attrs);
    peerEntry->setRibVersion(10);
  }

  /* Group announces a new prefix (kV4Prefix3) while peer 0 is detached */
  RibOutAnnouncementEntry entry3(
      kV4Prefix3,
      kDefaultPathID,
      peerInfo,
      attrs,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      false,
      std::chrono::system_clock::now(),
      100 /* ribVersion */);
  group_->processRibAnnouncedEntryForGroup(entry3);

  EXPECT_EQ(group_->getStats().getPostOutPrefixCount(), 3);
  EXPECT_EQ(group_->getStats().getPreOutPrefixCount(), 3);
  /* 4 + 1 (group announcement with 1 sync peer) = 5 */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 5);

  /* Peer 0 still thinks it has 2 prefixes */
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 2);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 2);

  /* Peer 0 rejoins — collapse should detect group-only entry for kV4Prefix3
   * and increment peer's counts. Clear PL to simulate that sendBgpUpdates was
   * run and packing list was drained. */
  adjRib0->clearPackingList();
  adjRib0->setPeerState(PeerUpdateState::DETACHED_READY_TO_JOIN);
  setUpReadyPeerConsumer(adjRib0);
  setGroupConsumerReady();
  group_->checkAndAcceptReadyToJoinPeers();

  /* Peer has discrepancy so it goes back to DETACHED_RUNNING, but
   * the stats should have been adjusted by the collapse logic */
  EXPECT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 3);
  EXPECT_EQ(adjRib0->getStats().getPreOutPrefixCount(), 3);
  EXPECT_EQ(
      adjRib0->getStats().getPostOutPrefixCount(),
      group_->getStats().getPostOutPrefixCount());
  EXPECT_EQ(
      adjRib0->getStats().getPreOutPrefixCount(),
      group_->getStats().getPreOutPrefixCount());
  /* 5 + 1 (collapse announcement for 1 peer) = 6 */
  EXPECT_EQ(totalSentPrefixCount, totalSentBefore + 6);
}

/*
 * Verify detachPeer sets all expected state:
 * 1. Egress prefix counts copied from group
 * 2. Packing list cloned
 * 3. Peer marked as detached (in detachedPeers_, sync bit cleared)
 * 4. Version fields inherited from group
 * 5. Blocked bitmap cleared
 * 6. Slow peer duration timer cancelled
 * 7. Detached consumer registered
 * 8. EoR state propagated if group has pending EoR
 */
TEST_F(UpdateGroupDetachLifecycleTest, DetachPeerSetsAllExpectedState) {
  auto adjRib0 = createAndRegisterPeer(0);
  auto adjRib1 = createAndRegisterPeer(1);
  setUpJoinedRunningPeer(adjRib0, 0);
  setUpJoinedRunningPeer(adjRib1, 1);

  group_->setLastSeenRibVersion(42);

  // Set up some group prefix counts
  group_->stats_.incrementPostOutPrefixCount(true);
  group_->stats_.incrementPostOutPrefixCount(true);
  group_->stats_.incrementPreOutPrefixCount(true);
  group_->stats_.incrementPreOutPrefixCount(true);

  // Mark peer as blocked in the group bitmap
  BitmapUtils::setBit(group_->adjRibBlockedBitmap_, 0);

  // Set group EoR pending
  group_->egressEoRsPending_ = true;

  // Add prefix to group PL so clonePackingList has something to clone
  auto attrs = std::make_shared<BgpPath>(*buildBgpPathFields(1, 1, 0, 0));
  addPrefixToGroupPL(folly::CIDRNetwork{folly::IPAddress("10.0.0.0"), 24});

  // Preconditions
  ASSERT_TRUE(group_->isPeerInSync(0));
  ASSERT_FALSE(group_->getDetachedPeers().contains(adjRib0));
  ASSERT_TRUE(BitmapUtils::isBitSet(group_->adjRibBlockedBitmap_, 0));
  ASSERT_EQ(adjRib0->getStats().getPostOutPrefixCount(), 0);

  group_->detachPeer(adjRib0);

  // 1. Egress prefix counts copied from group
  EXPECT_EQ(
      adjRib0->getStats().getPostOutPrefixCount(),
      group_->getStats().getPostOutPrefixCount());
  EXPECT_EQ(
      adjRib0->getStats().getPreOutPrefixCount(),
      group_->getStats().getPreOutPrefixCount());

  // 2. Packing list cloned (peer has non-empty PL)
  EXPECT_FALSE(adjRib0->attrToPrefixMap_.empty());

  // 3. Peer marked as detached
  EXPECT_TRUE(group_->getDetachedPeers().contains(adjRib0));
  EXPECT_FALSE(group_->isPeerInSync(0));

  // 4. Version fields inherited
  EXPECT_EQ(adjRib0->getLastSeenRibVersion(), 42);
  EXPECT_EQ(adjRib0->getDetachedRibVersion(), 42);

  // 5. Blocked bitmap cleared
  EXPECT_FALSE(BitmapUtils::isBitSet(group_->adjRibBlockedBitmap_, 0));

  // 6. Slow peer duration timer cancelled
  EXPECT_FALSE(
      adjRib0->slowPeerDurationTimer_ &&
      adjRib0->slowPeerDurationTimer_->isScheduled());

  // 7. Detached consumer registered
  EXPECT_NE(adjRib0->getDetachedConsumer(), nullptr);

  // 8. EoR state propagated (group had egressEoRsPending_ = true)
  EXPECT_TRUE(adjRib0->egressEoRsPending_);

  // Peer 1 unaffected
  EXPECT_TRUE(group_->isPeerInSync(1));
  EXPECT_FALSE(group_->getDetachedPeers().contains(adjRib1));
}

} // namespace facebook::bgp
