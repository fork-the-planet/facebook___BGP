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

#include <fboss/fsdb/if/gen-cpp2/fsdb_common_types.h>
#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "fboss/fsdb/tests/utils/FsdbTestServer.h"
#include "fboss/fsdb/tests/utils/FsdbTestSubscriber.h"
#include "fboss/lib/CommonUtils.h"
#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"
#include "neteng/fboss/bgp/if/gen-cpp2/bgp_thrift_types.h"

#include <folly/ScopeGuard.h>
#include <folly/coro/BlockingWait.h>
#include <gtest/gtest.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

DECLARE_bool(publish_rib_to_fsdb);

using namespace facebook::bgp;
namespace bgp_thrift = facebook::neteng::fboss::bgp::thrift;
namespace bgp_attr = facebook::neteng::fboss::bgp_attr;

using facebook::fboss::fsdb::PublisherId;
using facebook::fboss::fsdb::PublisherIds;
using facebook::fboss::fsdb::test::FsdbTestServer;
using facebook::fboss::fsdb::test::FsdbTestSubscriber;

template <bool EnableFsdbPatchAPI>
struct TestParams {
  static constexpr auto enableFsdbPatchApi = EnableFsdbPatchAPI;
};

using FsdbSyncerTestTypes =
    ::testing::Types<TestParams<false>, TestParams<true>>;

template <typename TestParams>
class FsdbSyncerTests : public ::testing::Test {
 public:
  void SetUp() override {
    fsdbTestServer_ = std::make_unique<FsdbTestServer>();
    FLAGS_fsdbPort = fsdbTestServer_->getFsdbPort();
    FLAGS_publish_state_to_fsdb = true;
    FLAGS_publish_stats_to_fsdb = true;
    subscriber_ = std::make_unique<FsdbTestSubscriber>("test-subscriber");
    FLAGS_fsdb_publish_state_with_patches = TestParams::enableFsdbPatchApi;
    fsdbSyncer_ = std::make_unique<FsdbSyncer>();
  }

  void TearDown() override {
    subscriber_.reset();
    fsdbSyncer_.reset();
  }

  std::unique_ptr<FsdbSyncer> fsdbSyncer_;
  std::unique_ptr<FsdbTestServer> fsdbTestServer_;
  std::unique_ptr<FsdbTestSubscriber> subscriber_;
  std::vector<std::vector<std::string>> subscriptions_;
};

TYPED_TEST_SUITE(FsdbSyncerTests, FsdbSyncerTestTypes);

TYPED_TEST(FsdbSyncerTests, testConnection) {
  this->fsdbSyncer_->start();
  auto bgpPubId = PublisherId("bgpd");
  PublisherIds pubIds = {bgpPubId};
  std::vector<std::string> bgpPubPath = {"bgp"};
  WITH_RETRIES({
    auto publisherToInfo = folly::coro::blockingWait(
        this->fsdbTestServer_->serviceHandler().co_getOperPublisherInfos(
            std::make_unique<PublisherIds>(pubIds)));
    ASSERT_EVENTUALLY_GT(publisherToInfo->count(bgpPubId), 0);
    auto publisherInfo = publisherToInfo->at(bgpPubId);
    ASSERT_EVENTUALLY_EQ(publisherInfo.size(), 1);
    EXPECT_EVENTUALLY_EQ(*publisherInfo[0].publisherId(), bgpPubId);
    EXPECT_EVENTUALLY_EQ(*publisherInfo[0].path()->raw(), bgpPubPath);
    EXPECT_EVENTUALLY_FALSE(*publisherInfo[0].isStats());
  });
  this->fsdbSyncer_->stop();
}

TYPED_TEST(FsdbSyncerTests, testConfigPublish) {
  auto subscribedConfig = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().config());

  auto config = thrift::BgpConfig();
  config.hold_time() = 123;
  config.ucmp_width() = 10000;
  config.defaultCommandLineArgs() = {{"foo", "1"}, {"bar", "2"}};

  this->fsdbSyncer_->setConfig(config);
  this->fsdbSyncer_->start();

  WITH_RETRIES({
    auto configLk = subscribedConfig.rlock();
    ASSERT_EVENTUALLY_TRUE(configLk->has_value());
    EXPECT_EVENTUALLY_EQ((*configLk)->hold_time(), 123);
    EXPECT_EVENTUALLY_EQ((*configLk)->ucmp_width(), 10000);
    EXPECT_EVENTUALLY_EQ((*configLk)->defaultCommandLineArgs()->size(), 2);
  })

  // set empty config
  this->fsdbSyncer_->setConfig(thrift::BgpConfig());
  WITH_RETRIES({
    auto configLk = subscribedConfig.rlock();
    ASSERT_EVENTUALLY_TRUE(configLk->has_value());
    EXPECT_EVENTUALLY_EQ((*configLk)->defaultCommandLineArgs()->size(), 0);
  })

  this->fsdbSyncer_->stop();
}

TYPED_TEST(FsdbSyncerTests, testRibMapPublish) {
  FLAGS_publish_rib_to_fsdb = true;

  auto subscribedRibMap = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().ribMap());

  this->fsdbSyncer_->start();

  // Create test RIB entries
  const std::string prefix1 = "10.0.0.0/24";
  const std::string prefix2 = "20.0.0.0/24";

  bgp_thrift::TRibEntry ribEntry1;
  ribEntry1.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  ribEntry1.prefix()->num_bits() = 24;
  ribEntry1.prefix()->prefix_bin() = std::string("\x0a\x00\x00\x00", 4);
  ribEntry1.best_group() = "best";

  bgp_thrift::TRibEntry ribEntry2;
  ribEntry2.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  ribEntry2.prefix()->num_bits() = 24;
  ribEntry2.prefix()->prefix_bin() = std::string("\x14\x00\x00\x00", 4);
  ribEntry2.best_group() = "best";

  // Test 1: Add RIB entries using setRibMap
  std::map<std::string, bgp_thrift::TRibEntry> initialRib;
  initialRib[prefix1] = ribEntry1;
  initialRib[prefix2] = ribEntry2;
  this->fsdbSyncer_->setRibMap(initialRib);

  WITH_RETRIES({
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->size(), 2);
    ASSERT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix1) > 0);
    ASSERT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix2) > 0);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->at(prefix1).best_group(), "best");
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->at(prefix2).best_group(), "best");
  })

  // Test 2: Add a new entry using updateRibMap
  const std::string prefix3 = "30.0.0.0/24";
  bgp_thrift::TRibEntry ribEntry3;
  ribEntry3.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  ribEntry3.prefix()->num_bits() = 24;
  ribEntry3.prefix()->prefix_bin() = std::string("\x1e\x00\x00\x00", 4);
  ribEntry3.best_group() = "new_best";

  std::map<std::string, std::optional<bgp_thrift::TRibEntry>> addUpdate;
  addUpdate[prefix3] = ribEntry3;
  this->fsdbSyncer_->updateRibMap(addUpdate);

  WITH_RETRIES({
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->size(), 3);
    ASSERT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix3) > 0);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->at(prefix3).best_group(), "new_best");
  })

  // Test 3: Remove an entry using updateRibMap (std::nullopt)
  std::map<std::string, std::optional<bgp_thrift::TRibEntry>> removeUpdate;
  removeUpdate[prefix1] = std::nullopt;
  this->fsdbSyncer_->updateRibMap(removeUpdate);

  WITH_RETRIES({
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->size(), 2);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->count(prefix1), 0);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix2) > 0);
    EXPECT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix3) > 0);
  })

  // Test 4: Mixed update - add one entry and remove another
  const std::string prefix4 = "40.0.0.0/24";
  bgp_thrift::TRibEntry ribEntry4;
  ribEntry4.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  ribEntry4.prefix()->num_bits() = 24;
  ribEntry4.prefix()->prefix_bin() = std::string("\x28\x00\x00\x00", 4);
  ribEntry4.best_group() = "mixed_update";

  std::map<std::string, std::optional<bgp_thrift::TRibEntry>> mixedUpdate;
  mixedUpdate[prefix4] = ribEntry4; // add
  mixedUpdate[prefix2] = std::nullopt; // remove
  this->fsdbSyncer_->updateRibMap(mixedUpdate);

  WITH_RETRIES({
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->size(), 2);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->count(prefix2), 0);
    ASSERT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix3) > 0);
    ASSERT_EVENTUALLY_TRUE((*ribMapLk)->count(prefix4) > 0);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->at(prefix4).best_group(), "mixed_update");
  })

  this->fsdbSyncer_->stop();
}

TYPED_TEST(FsdbSyncerTests, testRibMapUpdateExistingEntry) {
  FLAGS_publish_rib_to_fsdb = true;

  auto subscribedRibMap = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().ribMap());

  this->fsdbSyncer_->start();

  const std::string prefix1 = "10.0.0.0/24";

  // Create initial entry with best_group "initial" and 1 path with LP=20
  bgp_thrift::TRibEntry initialEntry;
  initialEntry.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  initialEntry.prefix()->num_bits() = 24;
  initialEntry.prefix()->prefix_bin() = std::string("\x0a\x00\x00\x00", 4);
  initialEntry.best_group() = "initial";
  {
    bgp_thrift::TBgpPath path;
    path.local_pref() = 20;
    std::map<std::string, std::vector<bgp_thrift::TBgpPath>> pathGroups;
    pathGroups["default"] = {path};
    initialEntry.paths() = std::move(pathGroups);
  }

  std::map<std::string, bgp_thrift::TRibEntry> initialRib;
  initialRib[prefix1] = initialEntry;
  this->fsdbSyncer_->setRibMap(initialRib);

  WITH_RETRIES({
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->size(), 1);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->at(prefix1).best_group(), "initial");
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->at(prefix1).paths()->size(), 1);
    EXPECT_EVENTUALLY_EQ(
        (*ribMapLk)->at(prefix1).paths()->at("default").at(0).local_pref(), 20);
  })

  // Update the SAME prefix with different best_group and path data.
  // This exercises the incremental update path (updateRibMap) for an
  // existing key. The update should overwrite the existing entry.
  bgp_thrift::TRibEntry updatedEntry;
  updatedEntry.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  updatedEntry.prefix()->num_bits() = 24;
  updatedEntry.prefix()->prefix_bin() = std::string("\x0a\x00\x00\x00", 4);
  updatedEntry.best_group() = "updated";
  {
    bgp_thrift::TBgpPath path1;
    path1.local_pref() = 100;
    bgp_thrift::TBgpPath path2;
    path2.local_pref() = 80;
    std::map<std::string, std::vector<bgp_thrift::TBgpPath>> pathGroups;
    pathGroups["best"] = {path1, path2};
    updatedEntry.paths() = std::move(pathGroups);
  }

  std::map<std::string, std::optional<bgp_thrift::TRibEntry>> update;
  update[prefix1] = updatedEntry;
  this->fsdbSyncer_->updateRibMap(update);

  WITH_RETRIES({
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->size(), 1);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->at(prefix1).best_group(), "updated");
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->at(prefix1).paths()->count("default"), 0);
    ASSERT_EVENTUALLY_EQ((*ribMapLk)->at(prefix1).paths()->count("best"), 1);
    EXPECT_EVENTUALLY_EQ(
        (*ribMapLk)->at(prefix1).paths()->at("best").size(), 2);
    EXPECT_EVENTUALLY_EQ(
        (*ribMapLk)->at(prefix1).paths()->at("best").at(0).local_pref(), 100);
  })

  // Remove the prefix via updateRibMap with std::nullopt
  std::map<std::string, std::optional<bgp_thrift::TRibEntry>> removeUpdate;
  removeUpdate[prefix1] = std::nullopt;
  this->fsdbSyncer_->updateRibMap(removeUpdate);

  WITH_RETRIES({
    auto ribMapLk = subscribedRibMap.rlock();
    ASSERT_EVENTUALLY_TRUE(ribMapLk->has_value());
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->size(), 0);
    EXPECT_EVENTUALLY_EQ((*ribMapLk)->count(prefix1), 0);
  })

  this->fsdbSyncer_->stop();
}

TYPED_TEST(FsdbSyncerTests, testPartialDrainStatePublish) {
  auto subscribedState = this->subscriber_->subscribe(
      this->subscriber_->getRootStatePath().bgp().partialDrainState());

  this->fsdbSyncer_->start();
  /*
   * partialDrainState is an `optional` field on BgpData; when the publisher
   * sets it to nullptr, the subscriber observes !has_value(). If an
   * assertion below escapes before stop() runs, FsdbSyncManager's destructor
   * CHECKs that the syncer was stopped — the SCOPE_EXIT keeps TearDown safe
   * regardless of which assertion path the test takes.
   */
  SCOPE_EXIT {
    this->fsdbSyncer_->stop();
  };

  /*
   * Publish a non-empty TPartialDrainState mirroring what
   * Rib::prepareFibProgramming builds when a prefix enters partial drain:
   * device-summary populated, one drained prefix entry attached.
   */
  bgp_thrift::TPartialDrainState state;
  state.partial_drain_state()->is_partially_drained() = true;
  state.partial_drain_state()->num_affected_prefixes() = 1;
  state.partial_drain_state()->partial_drain_transition_count() = 1;

  bgp_thrift::TPartiallyDrainedPrefix drainedPrefix;
  drainedPrefix.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  drainedPrefix.prefix()->num_bits() = 24;
  drainedPrefix.prefix()->prefix_bin() = std::string("\x0a\x00\x00\x00", 4);
  drainedPrefix.min_capacity()->next_hop_count() = 3;
  drainedPrefix.current_capacity()->next_hop_count() = 1;
  state.drained_prefixes()->push_back(drainedPrefix);

  this->fsdbSyncer_->setPartialDrainState(std::make_optional(state));

  WITH_RETRIES({
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
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .min_capacity()
             ->next_hop_count_ref(),
        3);
    // current_capacity carries the same criterion as min_capacity — the
    // next_hop_count arm (current nexthop count) for this MNH-triggered drain.
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .current_capacity()
             ->next_hop_count_ref(),
        1);
  })

  /*
   * Now publish an updated state — prefix count goes to 2. Verifies
   * subsequent publishes overwrite the prior value. The 2nd prefix uses
   * the `agg_lbw_bps` variant of the TCapacity union (for both min_capacity
   * and current_capacity) instead of the `next_hop_count` arm, so this single
   * test exercises both union branches round-tripping through the FSDB
   * serialization layer — matching the two production branches in
   * RibDC::buildPartialDrainPrefixEntry (next_hop_count arm = threshold +
   * current count on MNH violation, agg_lbw_bps arm = threshold + current agg
   * LBW on LBW violation).
   */
  state.partial_drain_state()->num_affected_prefixes() = 2;
  bgp_thrift::TPartiallyDrainedPrefix drainedPrefix2;
  drainedPrefix2.prefix()->afi() = bgp_attr::TBgpAfi::AFI_IPV4;
  drainedPrefix2.prefix()->num_bits() = 24;
  drainedPrefix2.prefix()->prefix_bin() = std::string("\x14\x00\x00\x00", 4);
  constexpr int64_t kAggLbwBpsThreshold = 100LL * 1'000'000'000LL; // 100 Gbps
  // Current aggregate LBW sits below the threshold (that is why it drained).
  constexpr int64_t kAggLbwBpsCurrent = 50LL * 1'000'000'000LL; // 50 Gbps
  drainedPrefix2.min_capacity()->agg_lbw_bps() = kAggLbwBpsThreshold;
  drainedPrefix2.current_capacity()->agg_lbw_bps() = kAggLbwBpsCurrent;
  state.drained_prefixes()->push_back(drainedPrefix2);

  this->fsdbSyncer_->setPartialDrainState(std::make_optional(state));

  WITH_RETRIES({
    auto stateLk = subscribedState.rlock();
    ASSERT_EVENTUALLY_TRUE(stateLk->has_value());
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)->partial_drain_state()->num_affected_prefixes(), 2);
    ASSERT_EVENTUALLY_EQ((*stateLk)->drained_prefixes()->size(), 2);
    // 1st prefix kept the next_hop_count variant from the prior publish.
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(0)
             .min_capacity()
             ->next_hop_count_ref(),
        3);
    // 2nd prefix carries the LBW variant — proves the agg_lbw_bps union
    // arm survives FSDB serialization just like the next_hop_count arm above.
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(1)
             .min_capacity()
             ->agg_lbw_bps_ref(),
        kAggLbwBpsThreshold);
    // current_capacity carries the current aggregate LBW on the matching
    // agg_lbw_bps arm — round-trips through FSDB just like the threshold.
    EXPECT_EVENTUALLY_EQ(
        *(*stateLk)
             ->drained_prefixes()
             ->at(1)
             .current_capacity()
             ->agg_lbw_bps_ref(),
        kAggLbwBpsCurrent);
  })

  /*
   * Clear the state with std::nullopt — exercises the FsdbSyncer branch that
   * sets the FSDB ref to nullptr (used by Rib when drainedPrefixCount_ goes
   * back to 0). The subscriber observes the optional field becoming unset.
   */
  this->fsdbSyncer_->setPartialDrainState(std::nullopt);

  WITH_RETRIES({
    auto stateLk = subscribedState.rlock();
    EXPECT_EVENTUALLY_FALSE(stateLk->has_value());
  })
}
