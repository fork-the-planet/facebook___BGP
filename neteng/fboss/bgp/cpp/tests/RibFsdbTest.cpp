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

#include <folly/coro/BlockingWait.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#define RibBase_TEST_FRIENDS friend class RibFsdbFixture;

#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/tests/RibFsdbPolicyTestFixture.h"
#include "neteng/fboss/bgp/cpp/tests/RibPolicyUtils.h"

using namespace facebook::bgp;
using namespace facebook::bgp::rib_policy;
using namespace facebook::neteng::fboss::bgp::thrift;
using namespace std::chrono;
using ::testing::_;

namespace facebook {
namespace bgp {

TEST_F(RibFsdbFixture, RibEntryFsdbPublishTest) {
  FLAGS_publish_rib_to_fsdb = true;

  auto subscribedRibMap = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().ribMap());

  const auto kBatchTime = milliseconds(50);
  rib_->setFibBatchTime(kBatchTime);

  auto prefix1 = folly::IPAddress::createNetwork("10.0.0.0/24");
  auto prefix2 = folly::IPAddress::createNetwork("20.0.0.0/24");
  auto prefixBatch1 = PrefixPathIds{{prefix1, kDefaultPathID}};
  auto prefixBatch2 = PrefixPathIds{{prefix2, kDefaultPathID}};

  auto prefix1Str = folly::IPAddress::networkToString(prefix1);
  auto prefix2Str = folly::IPAddress::networkToString(prefix2);

  EXPECT_CALL(*rib_, prepareFibProgramming_()).Times(testing::AtLeast(1));
  EXPECT_CALL(*fib_, updateUnicastRoute_(_, _, _, _, _, _))
      .Times(testing::AtLeast(1));
  EXPECT_CALL(*fib_, program_(_)).Times(testing::AtLeast(1));

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();

  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch1, eBgpPeer1_, attr_);
  sendAnnouncement(prefixBatch2, eBgpPeer1_, attr_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->size(), 2);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix1Str) > 0);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix2Str) > 0);
  })

  fibFuture = fib_->getFibProgramFuture();
  sendWithdrawal(prefixBatch1, eBgpPeer1_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->size(), 1);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->count(prefix1Str), 0);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix2Str) > 0);
  })

  fibFuture = fib_->getFibProgramFuture();
  sendWithdrawal(prefixBatch2, eBgpPeer1_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->size(), 0);
  })
}

/*
 * The FSDB publish path emits a minimal TRibEntry: only prefix + best_path.
 * The heavy `paths` map (and best_group) are intentionally skipped because the
 * FSDB best-path consumer reads only best_path. Verifies that the entry
 * published for an announced prefix carries best_path (with is_best_path=true)
 * and an empty paths map / best_group.
 */
TEST_F(RibFsdbFixture, RibEntryFsdbPublishesBestPathOnly) {
  FLAGS_publish_rib_to_fsdb = true;

  auto subscribedRibMap = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().ribMap());

  rib_->setFibBatchTime(milliseconds(50));

  auto prefix = folly::IPAddress::createNetwork("10.0.0.0/24");
  auto prefixStr = folly::IPAddress::networkToString(prefix);
  auto prefixBatch = PrefixPathIds{{prefix, kDefaultPathID}};

  // fib_ is set up by the fixture; this guard satisfies null-safety analysis.
  if (fib_ == nullptr) {
    throw std::runtime_error("fib_ not initialized by fixture");
  }
  auto& fib = *fib_;
  EXPECT_CALL(*rib_, prepareFibProgramming_()).Times(testing::AtLeast(1));
  EXPECT_CALL(fib, updateUnicastRoute_(_, _, _, _, _, _))
      .Times(testing::AtLeast(1));
  EXPECT_CALL(fib, program_(_)).Times(testing::AtLeast(1));

  /*
   * Announce the prefix BEFORE EOR so the EOR-triggered fullSync includes it in
   * the publisher's initial sync. Announcing after EOR makes the syncer publish
   * an empty ribMap first and the prefix as a later incremental update -- a
   * race that can drop the subscriber's CONNECTED state under load.
   */
  sendAnnouncement(prefixBatch, eBgpPeer1_, attr_);
  auto fibFuture = fib.getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();

  WITH_RETRIES_N_TIMED(60, milliseconds(1000), {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_TRUE((*ribMapLk)->count(prefixStr) > 0);
  })

  /*
   * Inspect the published entry: best_path is set, but the full paths map and
   * best_group are not -- only prefix + best_path are published.
   */
  auto ribMapLk = subscribedRibMap.rlock();
  ASSERT_TRUE(ribMapLk->has_value());
  auto it = (*ribMapLk)->find(prefixStr);
  ASSERT_NE(it, (*ribMapLk)->end());
  const auto& entry = it->second;
  ASSERT_TRUE(entry.best_path().has_value());
  EXPECT_TRUE(entry.best_path()->is_best_path().value_or(false));
  EXPECT_TRUE(entry.paths()->empty());
  EXPECT_TRUE(entry.best_group()->empty());
}

/*
 * A best path that exists but is denied by CPS native criteria (or otherwise
 * not in the selected multipath set) has no publishable best_path, so
 * createBestPathOnlyTRibEntry returns nullopt and the prefix is withdrawn from
 * the published ribMap rather than published as a prefix-only entry. Drives a
 * real mnh-violation transition and verifies the prefix disappears from FSDB.
 */
TEST_F(RibFsdbFixture, RibEntryFsdbWithdrawnWhenBestPathDeniedByCps) {
  FLAGS_publish_rib_to_fsdb = true;

  auto subscribedRibMap = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().ribMap());

  rib_->setFibBatchTime(milliseconds(50));

  auto prefixStr = folly::IPAddress::networkToString(kV4Prefix1);
  auto prefixBatch = PrefixPathIds{{kV4Prefix1, kDefaultPathID}};

  // fib_ is set up by the fixture; this guard satisfies null-safety analysis.
  if (fib_ == nullptr) {
    throw std::runtime_error("fib_ not initialized by fixture");
  }
  auto& fib = *fib_;
  EXPECT_CALL(*rib_, prepareFibProgramming_()).Times(testing::AtLeast(1));
  EXPECT_CALL(fib, updateUnicastRoute_(_, _, _, _, _, _))
      .Times(testing::AtLeast(1));
  EXPECT_CALL(fib, program_(_)).Times(testing::AtLeast(1));

  /*
   * Announce the path BEFORE EOR so the initial sync publishes it directly
   * (avoids the empty-initial-sync-then-incremental race that can drop the
   * subscriber under load).
   */
  sendAnnouncement(prefixBatch, eBgpPeer1_, attr_);
  auto fibFuture = fib.getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();
  WITH_RETRIES_N_TIMED(60, milliseconds(1000), {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_TRUE((*ribMapLk)->count(prefixStr) > 0);
  })

  /*
   * Inject a CPS policy the single installed path violates (mnh=3, no drain).
   * The violation fails CPS native criteria and nullifies the best path, so
   * createBestPathOnlyTRibEntry returns nullopt and the prefix is withdrawn
   * from the published ribMap.
   */
  TPathSelector tPathSelector;
  tPathSelector.bgp_native_path_selection_min_nexthop() = 3;
  auto ribFuture = rib_->getRibPrepareFibProgrammingFuture();
  sendPathSelectionPolicySet(
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector));
  rib_->waitForPathSelectionPolicyUpdate();
  ribFuture.wait();

  WITH_RETRIES_N_TIMED(60, milliseconds(1000), {
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->count(prefixStr), 0);
  })
}

/*
 * End-to-end gating test for the new partial-drain FSDB publish path. Runs
 * the Rib through a real partial-drain transition (entry then exit) and
 * verifies that FsdbSyncer::setPartialDrainState is invoked with the
 * expected state by observing FSDB via a subscriber. This exercises both
 * branches added in Rib::prepareFibProgramming:
 *   - drainedPrefixCount_ > 0 → publish populated TPartialDrainState
 *   - drainedPrefixCount_ == 0 → publish std::nullopt (clear)
 *
 * Setup mirrors PartialDrainStatusReflectsRibState in RibTest.cpp, but
 * runs under RibFsdbFixture so a real FSDB syncer is wired into the Rib.
 */
TEST_F(RibFsdbFixture, PartialDrainStatePublishedToFsdbOnTransition) {
  auto subscribedState = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().partialDrainState());

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();
  rib_->setFibBatchTime(milliseconds(2));

  // Drain the initial-dump messages (RibInitialAnnouncementStart +
  // RibOutAnnouncement with initialDump=true).
  WITH_RETRIES({ ASSERT_EVENTUALLY_EQ(ribOutQ_.size(), 2); });
  REPEAT_N(2, folly::coro::blockingWait(ribOutQ_.pop()));

  // Install one path so any mnh > 1 policy violates → partial drain.
  auto prefixBatch = PrefixPathIds{{kV4Prefix1, kDefaultPathID}};
  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch, eBgpPeer1_, attr_);
  fibFuture.wait();

  WITH_RETRIES({ ASSERT_EVENTUALLY_GE(ribOutQ_.size(), 1); });
  folly::coro::blockingWait(ribOutQ_.pop());

  // Inject CPS policy: mnh=3 + drain_on_min_nexthop_violation. The single
  // installed path violates mnh=3, so the prefix enters partial drain and
  // drainedPrefixCount_ flips 0 → 1 → setPartialDrainState(state) fires.
  TPathSelector tPathSelector;
  tPathSelector.bgp_native_path_selection_min_nexthop() = 3;
  tPathSelector.drain_on_min_nexthop_violation() = true;

  auto ribFuture = rib_->getRibPrepareFibProgrammingFuture();
  sendPathSelectionPolicySet(
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector));
  rib_->waitForPathSelectionPolicyUpdate();
  ribFuture.wait();

  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_TRUE(
        *(*stateLk)->partial_drain_state()->is_partially_drained());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 1);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->partial_drain_transition_count(),
        1);
    /*
     * On a drain-status transition, RibDC::publishPartialDrainState builds
     * the drained-prefix snapshot on demand (getPartialDrainState scans
     * ribEntries_ for getIsPartialDrain() entries) and publishes it
     * alongside the device summary. One path was installed for kV4Prefix1
     * before the policy injection.
     *
     * min_capacity (TCapacity) carries the trigger criterion: this drain
     * was triggered by an MNH violation, so the union must hold
     * next_hop_count == 3 (the configured
     * bgp_native_path_selection_min_nexthop) and the agg_lbw_bps arm must NOT
     * be set. Pinned here so a future refactor of buildPartialDrainPrefixEntry
     * that mis-selects the union arm or forgets to plumb mnhThreshold_
     * end-to-end (RibPolicy → RibEntry → FSDB) fails this test.
     */
    ASSERT_EVENTUALLY_EQ((*stateLk)->drained_prefixes()->size(), 1);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->drained_prefixes()->at(0).min_capacity()->next_hop_count(),
        3);
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .min_capacity()
                                ->agg_lbw_bps()
                                .has_value());
    /*
     * current_capacity mirrors the trigger criterion: MNH drain → the
     * next_hop_count arm holds the current nexthop count (1), and the
     * agg_lbw_bps arm is unset. Pins buildPartialDrainPrefixEntry's arm
     * selection for the current-value union end-to-end.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .current_capacity()
             ->next_hop_count(),
        1);
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .current_capacity()
                                ->agg_lbw_bps()
                                .has_value());
  })

  // Withdraw the only path → entry has no routes → selectBestPath resets
  // isPartialDrain_ → drainedPrefixCount_ flips 1 → 0 →
  // setPartialDrainState(std::nullopt) fires (clear branch). Because
  // partialDrainState is an optional field on BgpData, the FSDB ref-to-
  // nullptr publish surfaces to the subscriber as !has_value().
  fibFuture = fib_->getFibProgramFuture();
  sendWithdrawal(prefixBatch, eBgpPeer1_);
  fibFuture.wait();

  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    EXPECT_EVENTUALLY_FALSE(stateLk->has_value());
  })
}

/*
 * End-to-end gating test for the LBW-violation branch of the FSDB
 * partial-drain publish. Mirrors PartialDrainStatePublishedToFsdbOnTransition
 * but triggers the drain via bgp_min_aggregate_lbw_bps instead of
 * bgp_native_path_selection_min_nexthop. Verifies the published
 * TPartiallyDrainedPrefix carries min_capacity.agg_lbw_bps() set
 * to the configured threshold (and the next_hop_count union arm unset).
 *
 * Pinned end-to-end so a future regression in
 * RibBase::buildPartialDrainPrefixEntry that mis-selects the union arm — or
 * a missed aggLbwBpsThreshold_ plumbing step (RibPolicy → RibEntry → FSDB)
 * — fails here.
 */
TEST_F(RibFsdbFixture, PartialDrainStatePublishedWithLbwThresholdOnTransition) {
  auto subscribedState = fsdbSubscriber_->subscribe(
      fsdbSubscriber_->getRootStatePath().bgp().partialDrainState());

  auto fibFuture = fib_->getFibProgramFuture();
  sendInitialPathComputation();
  fibFuture.wait();
  rib_->setFibBatchTime(milliseconds(2));

  // Drain the initial-dump messages (RibInitialAnnouncementStart +
  // RibOutAnnouncement with initialDump=true).
  WITH_RETRIES({ ASSERT_EVENTUALLY_EQ(ribOutQ_.size(), 2); });
  REPEAT_N(2, folly::coro::blockingWait(ribOutQ_.pop()));

  /*
   * Build an LBW-capable attribute path: 10 Gbps via non-transitive LBW
   * community on AS1. Any policy threshold above 10 Gbps will violate the
   * aggregate-LBW check after the single path is multipath-selected.
   * (RibPolicy.cpp converts community bytes/sec to bits/sec via `*8` and
   * sums across multipaths before comparing against bgpNativeMinAggLbwbps_.)
   */
  auto attrLbw =
      std::make_shared<facebook::bgp::BgpPath>(*buildBgpPathFields(4, 4, 4, 4));
  attrLbw->setNonTransitiveLbwExtCommunity(uint16_t(kLocalAs1), kLbw10G);
  attrLbw->publish();

  auto prefixBatch = PrefixPathIds{{kV4Prefix1, kDefaultPathID}};
  fibFuture = fib_->getFibProgramFuture();
  sendAnnouncement(prefixBatch, eBgpPeer1_, attrLbw);
  fibFuture.wait();

  WITH_RETRIES({ ASSERT_EVENTUALLY_GE(ribOutQ_.size(), 1); });
  folly::coro::blockingWait(ribOutQ_.pop());

  /*
   * Inject CPS policy with bgp_min_aggregate_lbw_bps = 100 Gbps and
   * drain_on_min_nexthop_violation = true. The single installed path
   * provides 10 Gbps, so the aggregate falls below 100 Gbps and the prefix
   * enters partial drain via the LBW branch.
   *
   * Critical: bgp_native_path_selection_min_nexthop must NOT be set. When
   * MNH is set, PathSelector::overrideMultipathSelection returns before
   * reaching the LBW check (see the MNH branch in RibPolicy.cpp), which
   * would route the drain through the MNH arm instead.
   */
  constexpr int64_t kAggLbwBpsThreshold = 100LL * 1'000'000'000LL; // 100 Gbps
  TPathSelector tPathSelector;
  tPathSelector.bgp_min_aggregate_lbw_bps() = kAggLbwBpsThreshold;
  tPathSelector.drain_on_min_nexthop_violation() = true;

  auto ribFuture = rib_->getRibPrepareFibProgrammingFuture();
  sendPathSelectionPolicySet(
      createTPathSelectionPolicyWithPathSelector({kV4Prefix1}, tPathSelector));
  rib_->waitForPathSelectionPolicyUpdate();
  ribFuture.wait();

  WITH_RETRIES_N(5, {
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_TRUE(
        *(*stateLk)->partial_drain_state()->is_partially_drained());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 1);
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->partial_drain_transition_count(),
        1);
    ASSERT_EVENTUALLY_EQ((*stateLk)->drained_prefixes()->size(), 1);
    /*
     * The drain trigger was LBW, so min_capacity (TCapacity) must hold
     * agg_lbw_bps == the configured threshold (100 Gbps) and the
     * next_hop_count arm must NOT be set. Mirrors the inverse assertion in
     * the MNH test above.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->drained_prefixes()->at(0).min_capacity()->agg_lbw_bps(),
        kAggLbwBpsThreshold);
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .min_capacity()
                                ->next_hop_count()
                                .has_value());
    /*
     * current_capacity mirrors the LBW criterion: the agg_lbw_bps arm holds
     * the CURRENT aggregate link bandwidth (not the threshold) — one path of
     * kLbw10G bytes/sec * 8 = 10 Gbps, computed on demand by
     * RibDC::buildPartialDrainPrefixEntry — and the next_hop_count arm is
     * unset. This is the behavioral evidence that the producer recomputes and
     * surfaces the current aggregate LBW, the gap yikailin's comment flagged.
     */
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .current_capacity()
             ->agg_lbw_bps(),
        static_cast<int64_t>(kLbw10G * 8));
    EXPECT_EVENTUALLY_FALSE((*stateLk)
                                ->drained_prefixes()
                                ->at(0)
                                .current_capacity()
                                ->next_hop_count()
                                .has_value());
  })
}

} // namespace bgp
} // namespace facebook
