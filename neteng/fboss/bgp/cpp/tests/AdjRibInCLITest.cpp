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

#define AdjRib_TEST_FRIENDS                                     \
  friend class AdjRibInboundFixture;                            \
  FRIEND_TEST(AdjRibInboundFixture, V4GetNetworks2_Received);   \
  FRIEND_TEST(AdjRibInboundFixture, V4GetNetworks2_Advertised); \
  FRIEND_TEST(AdjRibInboundFixture, V6GetNetworks2);            \
  FRIEND_TEST(AdjRibInboundFixture, ConvertEntryToPathTest);

#include <folly/IPAddress.h>
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>

#include "neteng/fboss/bgp/cpp/BgpServiceUtil.h"
#include "neteng/fboss/bgp/cpp/adjrib/AdjRib.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/tests/AdjRibInUtils.h"
#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/PolicyUtils.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

using namespace facebook::bgp;
using namespace facebook::nettools::bgplib;
using namespace facebook::neteng::fboss::bgp::thrift;
using namespace facebook::neteng::fboss::bgp_attr;
using namespace folly::fibers;

using folly::IPAddress;

using bgp_policy::BgpPolicyActionType;

namespace facebook::bgp {

using ::testing::ElementsAre;

// Ensure that getNetworks works properly
TEST_F(AdjRibInboundFixture, V4GetNetworks) {
  setupAdjRib();

  fm_->addTask([&] {
    auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    adjRibInQ_->fiberPush(std::move(update));
  });

  fm_->addTask([&] {
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");

    std::map<TIpPrefix, TBgpPath> prefixToPath;
    adjRib_->getNetworks(prefixToPath, RouteFilterType::PRE_FILTER_RECEIVED);

    // Verify various fields returned
    EXPECT_EQ(prefixToPath.size(), 1);
    for (auto& [tPrefix, tPath] : prefixToPath) {
      EXPECT_EQ(TBgpAfi::AFI_IPV4, tPrefix.afi().value());
      EXPECT_EQ(kV4Prefix1.second, tPrefix.num_bits().value());

      auto binAddr = toBinaryAddress(kV4Prefix1.first);
      EXPECT_EQ(binAddr.addr()->toStdString(), tPrefix.prefix_bin().value());

      auto tNh = tPath.next_hop().value();
      EXPECT_EQ(TBgpAfi::AFI_IPV4, tNh.afi().value());
      EXPECT_EQ(kV4NexthopPrefixLen, tNh.num_bits().value());
      auto nhAddr = toBinaryAddress(kV4Nexthop1);
      EXPECT_EQ(nhAddr.addr()->toStdString(), tNh.prefix_bin().value());

      EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
      EXPECT_EQ(0, *apache::thrift::get_pointer(tPath.origin()));
      EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
      EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));
      EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.originator_id()));
      EXPECT_EQ(
          kOriginatorId, *apache::thrift::get_pointer(tPath.originator_id()));
      EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.cluster_list()));
      auto clusterList = *apache::thrift::get_pointer(tPath.cluster_list());
      ASSERT_EQ(1, clusterList.size());
      EXPECT_EQ(kOriginatorId, clusterList[0]);

      auto tSegs = tPath.as_path().value();
      EXPECT_EQ(1, tSegs.size());
      for (auto& tSeg : tSegs) {
        EXPECT_EQ(TAsPathSegType::AS_SEQUENCE, tSeg.seg_type().value());
        auto asns = tSeg.asns().value();
        ASSERT_EQ(1, asns.size());
        EXPECT_EQ(kAsSeqAsNum, asns[0]);
      }

      auto tComms = apache::thrift::get_pointer(tPath.communities());
      EXPECT_NE(nullptr, tComms);
      ASSERT_EQ(1, tComms->size());

      EXPECT_EQ(kCommAsNum, (*tComms)[0].asn().value());
      EXPECT_EQ(kCommAsVal, (*tComms)[0].value().value());
      EXPECT_EQ(
          ((int64_t)kCommAsNum << 16) + kCommAsVal,
          (*tComms)[0].community().value());

      auto tExtComms = apache::thrift::get_pointer(tPath.extCommunities());
      EXPECT_NE(nullptr, tExtComms);
      ASSERT_EQ(3, tExtComms->size());
      EXPECT_EQ(
          kExtCommASTypeFirstWord >> 24,
          ((*tExtComms)[0].u().value().get_two_byte_asn().type().value()));
      EXPECT_EQ(
          kExtCommASTypeFirstWord >> 16 & 0xFF,
          ((*tExtComms)[0].u().value().get_two_byte_asn().sub_type().value()));
      EXPECT_EQ(
          kExtCommASTypeFirstWord & 0xFFFF,
          ((*tExtComms)[0].u().value().get_two_byte_asn().asn().value()));
      EXPECT_EQ(
          kExtCommASTypeSecondWord,
          ((*tExtComms)[0].u().value().get_two_byte_asn().value().value()));

      EXPECT_EQ(
          kExtCommLbwTypeFirstWord >> 24,
          ((*tExtComms)[1].u().value().get_two_byte_asn().type().value()));
      EXPECT_EQ(
          kExtCommLbwTypeFirstWord >> 16 & 0xFF,
          ((*tExtComms)[1].u().value().get_two_byte_asn().sub_type().value()));
      EXPECT_EQ(
          kExtCommLbwTypeFirstWord & 0xFFFF,
          ((*tExtComms)[1].u().value().get_two_byte_asn().asn().value()));
      EXPECT_EQ(
          kExtCommLbwTypeSecondWord10G,
          ((*tExtComms)[1].u().value().get_two_byte_asn().value().value()));
    }
    terminateAdjRib();
  });

  evb_.loop();
}

// Ensure that getNetworks works properly
TEST_F(AdjRibInboundFixture, V4GetNetworks2_ReceivedNonAddPath) {
  setupAdjRib();

  folly::fibers::Baton baton;

  fm_->addTask([&] {
    auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    adjRibInQ_->fiberPush(std::move(update));

    update = createV4BgpUpdateSingleAnnounce(kV4Prefix2, kV4Nexthop2);
    adjRibInQ_->fiberPush(std::move(update));
  });
  folly::F14FastMap<uint32_t, folly::CIDRNetwork> prefixes = {
      {0, kV4Prefix1}, {1, kV4Prefix2}};
  folly::F14FastMap<uint32_t, folly::IPAddress> nhs = {
      {0, kV4Nexthop1}, {1, kV4Nexthop2}};

  fm_->addTask([&] {
    // wait for two mesages.
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    std::map<TIpPrefix, std::vector<TBgpPath>> prefixToPath;

    adjRib_->getNetworks2(prefixToPath, RouteFilterType::PRE_FILTER_RECEIVED);
    // Verify various fields returned
    EXPECT_EQ(prefixToPath.size(), 2);
    auto j = 0;
    for (auto& itrdict : prefixToPath) {
      EXPECT_EQ(itrdict.second.size(), 1);
      auto tPrefix = itrdict.first;
      for (int i = 0; i < itrdict.second.size(); i++) {
        auto tPath = itrdict.second[i];
        EXPECT_EQ(TBgpAfi::AFI_IPV4, tPrefix.afi().value());
        EXPECT_EQ(prefixes[j].second, tPrefix.num_bits().value());

        auto binAddr = toBinaryAddress(prefixes[j].first);
        EXPECT_EQ(binAddr.addr()->toStdString(), tPrefix.prefix_bin().value());

        auto tNh = tPath.next_hop().value();
        EXPECT_EQ(TBgpAfi::AFI_IPV4, tNh.afi().value());
        EXPECT_EQ(kV4NexthopPrefixLen, tNh.num_bits().value());
        auto nhAddr = toBinaryAddress(nhs[j]);
        EXPECT_EQ(nhAddr.addr()->toStdString(), tNh.prefix_bin().value());
        j = j + 1;

        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
        EXPECT_EQ(0, *apache::thrift::get_pointer(tPath.origin()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
        EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.originator_id()));
        EXPECT_EQ(
            kOriginatorId, *apache::thrift::get_pointer(tPath.originator_id()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.cluster_list()));
        auto clusterList = *apache::thrift::get_pointer(tPath.cluster_list());
        ASSERT_EQ(1, clusterList.size());
        EXPECT_EQ(kOriginatorId, clusterList[0]);

        auto tComms = apache::thrift::get_pointer(tPath.communities());
        EXPECT_NE(nullptr, tComms);
        ASSERT_EQ(1, tComms->size());

        EXPECT_EQ(kCommAsNum, (*tComms)[0].asn().value());
        EXPECT_EQ(kCommAsVal, (*tComms)[0].value().value());
        EXPECT_EQ(
            ((int64_t)kCommAsNum << 16) + kCommAsVal,
            (*tComms)[0].community().value());

        auto tExtComms = apache::thrift::get_pointer(tPath.extCommunities());
        EXPECT_NE(nullptr, tExtComms);
        ASSERT_EQ(3, tExtComms->size());
        EXPECT_EQ(
            kExtCommASTypeFirstWord >> 24,
            ((*tExtComms)[0].u().value().get_two_byte_asn().type().value()));
        EXPECT_EQ(
            kExtCommASTypeFirstWord >> 16 & 0xFF,
            ((*tExtComms)[0]
                 .u()
                 .value()
                 .get_two_byte_asn()
                 .sub_type()
                 .value()));
        EXPECT_EQ(
            kExtCommASTypeFirstWord & 0xFFFF,
            ((*tExtComms)[0].u().value().get_two_byte_asn().asn().value()));
        EXPECT_EQ(
            kExtCommASTypeSecondWord,
            ((*tExtComms)[0].u().value().get_two_byte_asn().value().value()));

        EXPECT_EQ(
            kExtCommLbwTypeFirstWord >> 24,
            ((*tExtComms)[1].u().value().get_two_byte_asn().type().value()));
        EXPECT_EQ(
            kExtCommLbwTypeFirstWord >> 16 & 0xFF,
            ((*tExtComms)[1]
                 .u()
                 .value()
                 .get_two_byte_asn()
                 .sub_type()
                 .value()));
        EXPECT_EQ(
            kExtCommLbwTypeFirstWord & 0xFFFF,
            ((*tExtComms)[1].u().value().get_two_byte_asn().asn().value()));
        EXPECT_EQ(
            kExtCommLbwTypeSecondWord10G,
            ((*tExtComms)[1].u().value().get_two_byte_asn().value().value()));
      }
    }
    terminateAdjRib();
  });

  evb_.loop();
}

// Ensure that getNetworks works properly
TEST_F(AdjRibInboundFixture, V4GetNetworks2_Received) {
  setupAdjRib();

  folly::fibers::Baton baton;

  folly::F14FastMap<int64_t, folly::IPAddress> pathIdToAddress = {
      {1, kV4Nexthop1}, {2, kV4Nexthop2}};

  fm_->addTask([&] {
    adjRib_->recAddPath_ = true;
    auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    for (auto& rigPrefix : *update->v4Announced2()) {
      rigPrefix.pathId() = 1;
    }
    adjRibInQ_->fiberPush(std::move(update));

    update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop2);
    for (auto& rigPrefix : *update->v4Announced2()) {
      rigPrefix.pathId() = 2;
    }
    adjRibInQ_->fiberPush(std::move(update));
  });

  fm_->addTask([&] {
    // wait for two mesages.
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    std::map<TIpPrefix, std::vector<TBgpPath>> prefixToPath;

    adjRib_->getNetworks2(prefixToPath, RouteFilterType::PRE_FILTER_RECEIVED);
    // Verify various fields returned
    EXPECT_EQ(prefixToPath.size(), 1);
    for (auto& itrdict : prefixToPath) {
      EXPECT_EQ(itrdict.second.size(), 2);
      auto tPrefix = itrdict.first;
      for (int i = 0; i < itrdict.second.size(); i++) {
        auto tPath = itrdict.second[i];
        EXPECT_EQ(TBgpAfi::AFI_IPV4, tPrefix.afi().value());
        EXPECT_EQ(kV4Prefix1.second, tPrefix.num_bits().value());

        auto binAddr = toBinaryAddress(kV4Prefix1.first);
        EXPECT_EQ(binAddr.addr()->toStdString(), tPrefix.prefix_bin().value());

        auto tNh = tPath.next_hop().value();
        EXPECT_EQ(TBgpAfi::AFI_IPV4, tNh.afi().value());
        EXPECT_EQ(kV4NexthopPrefixLen, tNh.num_bits().value());
        auto nhAddr = toBinaryAddress(pathIdToAddress.at(*tPath.path_id()));
        EXPECT_EQ(nhAddr.addr()->toStdString(), tNh.prefix_bin().value());

        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
        EXPECT_EQ(0, *apache::thrift::get_pointer(tPath.origin()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
        EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.originator_id()));
        EXPECT_EQ(
            kOriginatorId, *apache::thrift::get_pointer(tPath.originator_id()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.cluster_list()));
        auto clusterList = *apache::thrift::get_pointer(tPath.cluster_list());
        ASSERT_EQ(1, clusterList.size());
        EXPECT_EQ(kOriginatorId, clusterList[0]);

        auto tSegs = tPath.as_path().value();
        EXPECT_EQ(1, tSegs.size());
        for (auto& tSeg : tSegs) {
          EXPECT_EQ(TAsPathSegType::AS_SEQUENCE, tSeg.seg_type().value());
          auto asns = tSeg.asns().value();
          ASSERT_EQ(1, asns.size());
          EXPECT_EQ(kAsSeqAsNum, asns[0]);
        }

        auto tComms = apache::thrift::get_pointer(tPath.communities());
        EXPECT_NE(nullptr, tComms);
        ASSERT_EQ(1, tComms->size());

        EXPECT_EQ(kCommAsNum, (*tComms)[0].asn().value());
        EXPECT_EQ(kCommAsVal, (*tComms)[0].value().value());
        EXPECT_EQ(
            ((int64_t)kCommAsNum << 16) + kCommAsVal,
            (*tComms)[0].community().value());

        auto tExtComms = apache::thrift::get_pointer(tPath.extCommunities());
        EXPECT_NE(nullptr, tExtComms);
        ASSERT_EQ(3, tExtComms->size());
        EXPECT_EQ(
            kExtCommASTypeFirstWord >> 24,
            ((*tExtComms)[0].u().value().get_two_byte_asn().type().value()));
        EXPECT_EQ(
            kExtCommASTypeFirstWord >> 16 & 0xFF,
            ((*tExtComms)[0]
                 .u()
                 .value()
                 .get_two_byte_asn()
                 .sub_type()
                 .value()));
        EXPECT_EQ(
            kExtCommASTypeFirstWord & 0xFFFF,
            ((*tExtComms)[0].u().value().get_two_byte_asn().asn().value()));
        EXPECT_EQ(
            kExtCommASTypeSecondWord,
            ((*tExtComms)[0].u().value().get_two_byte_asn().value().value()));

        EXPECT_EQ(
            kExtCommLbwTypeFirstWord >> 24,
            ((*tExtComms)[1].u().value().get_two_byte_asn().type().value()));
        EXPECT_EQ(
            kExtCommLbwTypeFirstWord >> 16 & 0xFF,
            ((*tExtComms)[1]
                 .u()
                 .value()
                 .get_two_byte_asn()
                 .sub_type()
                 .value()));
        EXPECT_EQ(
            kExtCommLbwTypeFirstWord & 0xFFFF,
            ((*tExtComms)[1].u().value().get_two_byte_asn().asn().value()));
        EXPECT_EQ(
            kExtCommLbwTypeSecondWord10G,
            ((*tExtComms)[1].u().value().get_two_byte_asn().value().value()));
      }
    }
    terminateAdjRib();
  });

  evb_.loop();
}

// Ensure that getNetworks works properly
TEST_F(AdjRibInboundFixture, V4GetNetworks2_AdvertisedNonAddPath) {
  setupAdjRib();

  folly::fibers::Baton baton;

  fm_->addTask([&] {
    auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    adjRibInQ_->fiberPush(std::move(update));

    update = createV4BgpUpdateSingleAnnounce(kV4Prefix2, kV4Nexthop2);
    adjRibInQ_->fiberPush(std::move(update));
  });
  folly::F14FastMap<uint32_t, folly::CIDRNetwork> prefixes = {
      {0, kV4Prefix1}, {1, kV4Prefix2}};
  folly::F14FastMap<uint32_t, folly::IPAddress> nhs = {
      {0, kV4Nexthop1}, {1, kV4Nexthop2}};

  fm_->addTask([&] {
    // wait for two mesages.
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    std::map<TIpPrefix, std::vector<TBgpPath>> prefixToPath;

    adjRib_->getNetworks2(prefixToPath, RouteFilterType::POST_FILTER_RECEIVED);
    // Verify various fields returned
    EXPECT_EQ(prefixToPath.size(), 2);
    auto j = 0;
    for (auto& itrdict : prefixToPath) {
      EXPECT_EQ(itrdict.second.size(), 1);
      auto tPrefix = itrdict.first;
      for (int i = 0; i < itrdict.second.size(); i++) {
        auto tPath = itrdict.second[i];
        EXPECT_EQ(TBgpAfi::AFI_IPV4, tPrefix.afi().value());
        EXPECT_EQ(prefixes[j].second, tPrefix.num_bits().value());

        auto binAddr = toBinaryAddress(prefixes[j].first);
        EXPECT_EQ(binAddr.addr()->toStdString(), tPrefix.prefix_bin().value());

        auto tNh = tPath.next_hop().value();
        EXPECT_EQ(TBgpAfi::AFI_IPV4, tNh.afi().value());
        EXPECT_EQ(kV4NexthopPrefixLen, tNh.num_bits().value());
        auto nhAddr = toBinaryAddress(nhs[j]);
        EXPECT_EQ(nhAddr.addr()->toStdString(), tNh.prefix_bin().value());
        j = j + 1;

        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
        EXPECT_EQ(0, *apache::thrift::get_pointer(tPath.origin()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
        EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.originator_id()));
        EXPECT_EQ(
            kOriginatorId, *apache::thrift::get_pointer(tPath.originator_id()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.cluster_list()));
        auto clusterList = *apache::thrift::get_pointer(tPath.cluster_list());
        ASSERT_EQ(1, clusterList.size());
        EXPECT_EQ(kOriginatorId, clusterList[0]);

        auto tComms = apache::thrift::get_pointer(tPath.communities());
        EXPECT_NE(nullptr, tComms);
        ASSERT_EQ(1, tComms->size());

        EXPECT_EQ(kCommAsNum, (*tComms)[0].asn().value());
        EXPECT_EQ(kCommAsVal, (*tComms)[0].value().value());
        EXPECT_EQ(
            ((int64_t)kCommAsNum << 16) + kCommAsVal,
            (*tComms)[0].community().value());

        auto tExtComms = apache::thrift::get_pointer(tPath.extCommunities());
        EXPECT_NE(nullptr, tExtComms);
        ASSERT_EQ(3, tExtComms->size());
        EXPECT_EQ(
            kExtCommASTypeFirstWord >> 24,
            ((*tExtComms)[0].u().value().get_two_byte_asn().type().value()));
        EXPECT_EQ(
            kExtCommASTypeFirstWord >> 16 & 0xFF,
            ((*tExtComms)[0]
                 .u()
                 .value()
                 .get_two_byte_asn()
                 .sub_type()
                 .value()));
        EXPECT_EQ(
            kExtCommASTypeFirstWord & 0xFFFF,
            ((*tExtComms)[0].u().value().get_two_byte_asn().asn().value()));
        EXPECT_EQ(
            kExtCommASTypeSecondWord,
            ((*tExtComms)[0].u().value().get_two_byte_asn().value().value()));

        EXPECT_EQ(
            kExtCommLbwTypeFirstWord >> 24,
            ((*tExtComms)[1].u().value().get_two_byte_asn().type().value()));
        EXPECT_EQ(
            kExtCommLbwTypeFirstWord >> 16 & 0xFF,
            ((*tExtComms)[1]
                 .u()
                 .value()
                 .get_two_byte_asn()
                 .sub_type()
                 .value()));
        EXPECT_EQ(
            kExtCommLbwTypeFirstWord & 0xFFFF,
            ((*tExtComms)[1].u().value().get_two_byte_asn().asn().value()));
        EXPECT_EQ(
            kExtCommLbwTypeSecondWord10G,
            ((*tExtComms)[1].u().value().get_two_byte_asn().value().value()));
      }
    }
    terminateAdjRib();
  });

  evb_.loop();
}

TEST_F(AdjRibInboundFixture, V4GetNetworks2_Advertised) {
  setupAdjRib();

  fm_->addTask([&] {
    adjRib_->recAddPath_ = true;
    auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    adjRibInQ_->fiberPush(std::move(update));
  });

  fm_->addTask([&] {
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");

    std::map<TIpPrefix, std::vector<TBgpPath>> prefixToPath;
    uint32_t pathId = 123;
    auto prefix = kV4Prefix1;
    auto inputUpdate = createV4BgpUpdateSingleAnnounce(prefix, kV4Nexthop1);
    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(*inputUpdate)));

    adjRib_->adjRibOutGroup_->addToLiteTree(
        adjRib_->adjRibOutGroup_->LiteTree_,
        prefix,
        adjRib_->getPeerOwnerKey(),
        pathId);
    auto adjRibEntry = adjRib_->adjRibOutGroup_->getFromLiteTree(
        adjRib_->adjRibOutGroup_->LiteTree_,
        prefix,
        adjRib_->getPeerOwnerKey());

    adjRibEntry->setPreOut(inputAttrs);
    adjRib_->getNetworks2(prefixToPath, RouteFilterType::PRE_FILTER_ADVERTISED);
    EXPECT_EQ(prefixToPath.size(), 1);
    for (auto& itrdict : prefixToPath) {
      EXPECT_EQ(itrdict.second.size(), 1);
    }
    terminateAdjRib();
  });

  evb_.loop();
}

// Ensure that getNetworks works properly
TEST_F(AdjRibInboundFixture, V6GetNetworks) {
  setupAdjRib();

  std::vector<folly::CIDRNetwork> prefixSet{kV6Prefix1};

  fm_->addTask([&] {
    auto update = createV6BgpUpdateMultipleAnnounce(prefixSet);
    adjRibInQ_->fiberPush(std::move(update));
  });

  fm_->addTask([&] {
    auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");

    std::map<TIpPrefix, TBgpPath> prefixToPath;
    adjRib_->getNetworks(prefixToPath, RouteFilterType::PRE_FILTER_RECEIVED);
    // Verify various fields returned
    EXPECT_EQ(1, prefixToPath.size());
    for (auto& [tPrefix, tPath] : prefixToPath) {
      EXPECT_EQ(TBgpAfi::AFI_IPV6, tPrefix.afi().value());
      EXPECT_EQ(kV6Prefix1.second, tPrefix.num_bits().value());

      auto binAddr = toBinaryAddress(kV6Prefix1.first);
      EXPECT_EQ(binAddr.addr()->toStdString(), tPrefix.prefix_bin().value());

      auto tNh = tPath.next_hop().value();
      EXPECT_EQ(TBgpAfi::AFI_IPV6, tNh.afi().value());
      EXPECT_EQ(kV6NexthopPrefixLen, tNh.num_bits().value());
      auto nhAddr = toBinaryAddress(kV6Nexthop1);
      EXPECT_EQ(nhAddr.addr()->toStdString(), tNh.prefix_bin().value());

      EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
      EXPECT_EQ(0, *apache::thrift::get_pointer(tPath.origin()));
      EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
      EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));

      auto tSegs = tPath.as_path().value();
      EXPECT_EQ(1, tSegs.size());
      for (auto& tSeg : tSegs) {
        EXPECT_EQ(TAsPathSegType::AS_SEQUENCE, tSeg.seg_type().value());
        auto asns = tSeg.asns().value();
        ASSERT_EQ(1, asns.size());
        EXPECT_EQ(kAsSeqAsNum, asns[0]);
      }
    }
    terminateAdjRib();
  });

  evb_.loop();
}

// Ensure that getNetworks works properly
TEST_F(AdjRibInboundFixture, V6GetNetworks2) {
  setupAdjRib();

  std::vector<folly::CIDRNetwork> prefixSet{kV6Prefix1};

  folly::F14FastMap<int64_t, folly::IPAddress> pathIdToAddress = {
      {1, kV6Nexthop1}, {2, kV6Nexthop2}};

  fm_->addTask([&] {
    adjRib_->recAddPath_ = true;
    auto update = createV6BgpUpdateMultipleAnnounce(prefixSet);
    for (auto& rigPrefix : *update->mpAnnounced()->prefixes()) {
      rigPrefix.pathId() = 1;
    }
    adjRibInQ_->fiberPush(std::move(update));

    update = createV6BgpUpdateMultipleAnnounce(prefixSet);
    for (auto& rigPrefix : *update->mpAnnounced()->prefixes()) {
      rigPrefix.pathId() = 2;
    }
    *update->mpAnnounced()->nexthop() = toBinaryAddress(kV6Nexthop2);
    adjRibInQ_->fiberPush(std::move(update));
  });

  fm_->addTask([&] {
    auto msg = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");
    auto msg2 = facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");

    std::map<TIpPrefix, std::vector<TBgpPath>> prefixToPath;
    adjRib_->getNetworks2(prefixToPath, RouteFilterType::PRE_FILTER_RECEIVED);

    // Verify various fields returned
    EXPECT_EQ(1, prefixToPath.size());
    for (auto& itrdict : prefixToPath) {
      auto tPrefix = itrdict.first;
      EXPECT_EQ(itrdict.second.size(), 2);
      for (int i = 0; i < itrdict.second.size(); i++) {
        auto tPath = itrdict.second[i];

        EXPECT_EQ(TBgpAfi::AFI_IPV6, tPrefix.afi().value());
        EXPECT_EQ(kV6Prefix1.second, tPrefix.num_bits().value());

        auto binAddr = toBinaryAddress(kV6Prefix1.first);
        EXPECT_EQ(binAddr.addr()->toStdString(), tPrefix.prefix_bin().value());

        auto tNh = tPath.next_hop().value();
        EXPECT_EQ(TBgpAfi::AFI_IPV6, tNh.afi().value());
        EXPECT_EQ(kV6NexthopPrefixLen, tNh.num_bits().value());
        auto nhAddr = toBinaryAddress(pathIdToAddress.at(*tPath.path_id()));
        EXPECT_EQ(nhAddr.addr()->toStdString(), tNh.prefix_bin().value());

        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
        EXPECT_EQ(0, *apache::thrift::get_pointer(tPath.origin()));
        EXPECT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
        EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));

        auto tSegs = tPath.as_path().value();
        EXPECT_EQ(1, tSegs.size());
        for (auto& tSeg : tSegs) {
          EXPECT_EQ(TAsPathSegType::AS_SEQUENCE, tSeg.seg_type().value());
          auto asns = tSeg.asns().value();
          ASSERT_EQ(1, asns.size());
          EXPECT_EQ(kAsSeqAsNum, asns[0]);
        }
      }
    }
    terminateAdjRib();
  });

  evb_.loop();
}

// Ensure that getDryRunNetworks works properly
// Setup adjRib with startup config/policy kIngressPolicyName which matches
// all prefixes and sets origin action IGP.
// Input bgp update with 2 prefixes p1, p2 with origin EGP
// Input to getDryRunNetworks bgpcpp-dryrun.conf which blocks one prefix(p1)
// and modifies another prefix(p2) to origin INCOMPLETE Verify that with dry
// run we see prefix p1 blocked and prefix p2 action modified to INCOMPLETE
TEST_F(AdjRibInboundFixture, getDryRunNetworks) {
  // Create a policy with one term
  // Term1 match all, set action origin IGP
  const std::string policyName = kIngressPolicyName;
  auto policyManager = setupMatchAllSetOriginIgpPolicy(policyName);
  std::vector<folly::CIDRNetwork> prefixSet{kV4Prefix1, kV4Prefix2};

  setupAdjRib(policyManager, policyName);

  fm_->addTask([&] {
    auto update = createV4BgpUpdateMultipleAnnounce(
        prefixSet, BgpAttrOrigin::BGP_ORIGIN_EGP);
    adjRibInQ_->fiberPush(std::move(update));
  });

  fm_->addTask([&] {
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");

    std::map<TIpPrefix, TBgpPath> prefixToPath;
    adjRib_->getNetworks(prefixToPath, RouteFilterType::POST_FILTER_RECEIVED);
    // Verify that adjRib has learnt both prefixes and are
    // returned with proper values after applying init config(policy) by
    // getNetworks
    EXPECT_EQ(prefixToPath.size(), 2);
    for (auto& prefix : prefixSet) {
      TIpPrefix tPrefix;
      tPrefix.afi() = TBgpAfi::AFI_IPV4;
      tPrefix.num_bits() = prefix.second;
      auto binAddr = toBinaryAddress(prefix.first);
      tPrefix.prefix_bin() = binAddr.addr()->toStdString();

      ASSERT_NE(prefixToPath.find(tPrefix), prefixToPath.end());
      auto tPath = prefixToPath[tPrefix];
      ASSERT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
      // Verify that both prefixes have origin IGP
      ASSERT_EQ(
          static_cast<int>(BgpAttrOrigin::BGP_ORIGIN_IGP),
          *apache::thrift::get_pointer(tPath.origin()));

      ASSERT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
      EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));
      ASSERT_NE(nullptr, apache::thrift::get_pointer(tPath.originator_id()));
      EXPECT_EQ(
          kOriginatorId, *apache::thrift::get_pointer(tPath.originator_id()));
    }

    // Verify that getDryRunNetworks modified the result based on dry run
    // policy kV4Prefix1 is discarded kV4Prefix2 has origin modified to
    // INCOMPELTE
    std::string configFile =
        "neteng/fboss/bgp/cpp/tests/sample_configs/bgpcpp-dryrun.conf";
    auto configFilePath = getAbsoluteFilePath(configFile);
    prefixToPath.clear();

    adjRib_->getDryRunNetworks(
        prefixToPath,
        std::make_unique<std::string>(configFilePath),
        RouteFilterType::POST_FILTER_RECEIVED);
    EXPECT_EQ(prefixToPath.size(), 1);

    // Verify only kV4Prefix2 is returned
    TIpPrefix tPrefix;
    tPrefix.afi() = TBgpAfi::AFI_IPV4;
    tPrefix.num_bits() = kV4Prefix2.second;
    auto binAddr = toBinaryAddress(kV4Prefix2.first);
    tPrefix.prefix_bin() = binAddr.addr()->toStdString();

    ASSERT_NE(prefixToPath.find(tPrefix), prefixToPath.end());
    auto tPath = prefixToPath[tPrefix];
    ASSERT_NE(nullptr, apache::thrift::get_pointer(tPath.origin()));
    // Verify that origin action is modified as per dry run policy
    ASSERT_EQ(
        static_cast<int>(BgpAttrOrigin::BGP_ORIGIN_INCOMPLETE),
        *apache::thrift::get_pointer(tPath.origin()));

    // Verify that other fields are not modified
    ASSERT_NE(nullptr, apache::thrift::get_pointer(tPath.local_pref()));
    EXPECT_EQ(kLocalPref, *apache::thrift::get_pointer(tPath.local_pref()));
    ASSERT_NE(nullptr, apache::thrift::get_pointer(tPath.originator_id()));
    EXPECT_EQ(
        kOriginatorId, *apache::thrift::get_pointer(tPath.originator_id()));

    // Verify that dry run handles case where all routes are not of filter
    // type requested. i.e. dry run policy won't be applied on any route
    {
      prefixToPath.clear();

      adjRib_->getDryRunNetworks(
          prefixToPath,
          std::make_unique<std::string>(configFilePath),
          RouteFilterType::POST_FILTER_ADVERTISED);
      EXPECT_EQ(prefixToPath.size(), 0);
    }
    terminateAdjRib();
  });

  evb_.loop();
}

TEST_F(AdjRibInboundFixture, ConvertEntryToPathTest) {
  setupAdjRib();

  fm_->addTask([&] {
    auto update = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    adjRibInQ_->fiberPush(std::move(update));
  });

  fm_->addTask([&] {
    facebook::bgp::test::boundedBlockingPop(ribInQ_, "ribInQ_");

    std::map<TIpPrefix, TBgpPath> prefixToPath;
    adjRib_->getNetworks(prefixToPath, RouteFilterType::PRE_FILTER_RECEIVED);
    terminateAdjRib();
  });

  fm_->addTask([&] {
    AdjRibEntry adjRibEntry(1);

    auto inputUpdate = createV4BgpUpdateSingleAnnounce(kV4Prefix1, kV4Nexthop1);
    inputUpdate->attrs()->localPref() = kLocalPref2;
    *inputUpdate->attrs()->originatorId() = kPeerAddr1.asV4().toLong();
    inputUpdate->attrs()->clusterList()->clear();
    inputUpdate->attrs()->clusterList()->push_back(kPeerAddr1.asV4().toLong());

    uint32_t pathId = 123;

    auto inputAttrs = std::make_shared<facebook::bgp::BgpPath>(
        BgpPathFields(*BgpUpdate2toBgpPathC(*inputUpdate)));

    TBgpPath path = createTBgpPath(*inputAttrs);

    auto postInPolicy = adjRibEntry.getPostInPolicy();
    if (postInPolicy) {
      path.policy_name() = *postInPolicy;
    }
    auto prefix = createTIpPrefix(kV4Prefix1);
    // nullify last_modified_time as it is not comparable
    path.last_modified_time() = 0;
    path.path_id() = pathId;

    // Cover the cases where the input Rib entry is empty
    EXPECT_EQ(
        adjRib_->convertEntryToPath(
            kV4Prefix1, adjRibEntry, RouteFilterType::PRE_FILTER_RECEIVED),
        std::nullopt);
    EXPECT_EQ(
        adjRib_->convertEntryToPath(
            kV4Prefix1, adjRibEntry, RouteFilterType::POST_FILTER_RECEIVED),
        std::nullopt);
    EXPECT_EQ(
        adjRib_->convertEntryToPath(
            kV4Prefix1, adjRibEntry, RouteFilterType::PRE_FILTER_ADVERTISED),
        std::nullopt);
    EXPECT_EQ(
        adjRib_->convertEntryToPath(
            kV4Prefix1, adjRibEntry, RouteFilterType::POST_FILTER_ADVERTISED),
        std::nullopt);

    adjRibEntry.setPreIn(inputAttrs);
    adjRibEntry.setPostAttr(inputAttrs);
    adjRibEntry.setPreOut(inputAttrs);
    adjRibEntry.setPostAttr(inputAttrs);

    auto preRcv = adjRib_->convertEntryToPath(
        kV4Prefix1, adjRibEntry, RouteFilterType::PRE_FILTER_RECEIVED, pathId);
    EXPECT_EQ(preRcv.value().first, prefix);
    auto path_comp = preRcv.value().second;
    path_comp.last_modified_time() = 0;
    EXPECT_EQ(path_comp, path);

    auto postRcv = adjRib_->convertEntryToPath(
        kV4Prefix1, adjRibEntry, RouteFilterType::POST_FILTER_RECEIVED, pathId);
    EXPECT_EQ(postRcv.value().first, prefix);
    path_comp = postRcv.value().second;
    path_comp.last_modified_time() = 0;
    EXPECT_EQ(path_comp, path);

    auto preAdv = adjRib_->convertEntryToPath(
        kV4Prefix1,
        adjRibEntry,
        RouteFilterType::PRE_FILTER_ADVERTISED,
        pathId);
    EXPECT_EQ(preAdv.value().first, prefix);
    path_comp = preAdv.value().second;
    path_comp.last_modified_time() = 0;
    EXPECT_EQ(path_comp, path);

    auto postAdv = adjRib_->convertEntryToPath(
        kV4Prefix1,
        adjRibEntry,
        RouteFilterType::POST_FILTER_ADVERTISED,
        pathId);
    EXPECT_EQ(postAdv.value().first, prefix);
    path_comp = postAdv.value().second;
    path_comp.last_modified_time() = 0;
    EXPECT_EQ(path_comp, path);
  });
  evb_.loop();
}

} // namespace facebook::bgp
