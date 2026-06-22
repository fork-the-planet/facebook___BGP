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

#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"

#include <chrono>

#include <gtest/gtest.h>

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Task.h>
#include <folly/synchronization/Baton.h>

#include "neteng/fboss/bgp/cpp/lib/coro/BackPressuredQueue.h"

namespace facebook::bgp::test {
namespace {

using TestQueue = nettools::bgplib::MonitoredBackPressuredQueue<int>;

TEST(BoundedWaitUtilsTest, BoundedBlockingPopReturnsValue) {
  TestQueue q{/*capacity=*/4};
  folly::coro::blockingWait(q.push(42));
  EXPECT_EQ(42, boundedBlockingPop(q, "q", std::chrono::seconds{1}));
}

TEST(BoundedWaitUtilsTest, BoundedBlockingPopTimesOutWithDescriptiveMessage) {
  TestQueue q{/*capacity=*/4};
  try {
    boundedBlockingPop(q, "myQueue", std::chrono::seconds{1});
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("myQueue"), std::string::npos) << what;
    EXPECT_NE(what.find("1s"), std::string::npos) << what;
    EXPECT_NE(what.find("timed out"), std::string::npos) << what;
  }
}

TEST(BoundedWaitUtilsTest, BoundedPopCoroReturnsValue) {
  TestQueue q{/*capacity=*/4};
  folly::coro::blockingWait(q.push(7));
  auto v =
      folly::coro::blockingWait(boundedPop(q, "q", std::chrono::seconds{1}));
  EXPECT_EQ(7, v);
}

TEST(BoundedWaitUtilsTest, BoundedPopCoroTimesOut) {
  TestQueue q{/*capacity=*/4};
  EXPECT_THROW(
      folly::coro::blockingWait(boundedPop(q, "q", std::chrono::seconds{1})),
      std::runtime_error);
}

TEST(BoundedWaitUtilsTest, BoundedBatonWaitReturnsWhenPosted) {
  /*
   * Post before wait — exercises the early-delivery fast path. The wait
   * returns immediately and try_wait_for never has to sleep, so the test
   * doesn't depend on timing.
   */
  folly::Baton<> baton;
  baton.post();
  boundedBatonWait(baton, "test", std::chrono::seconds{5});
  SUCCEED();
}

TEST(BoundedWaitUtilsTest, BoundedBatonWaitTimesOutWithDescriptiveMessage) {
  folly::Baton<> baton;
  try {
    boundedBatonWait(baton, "myBaton", std::chrono::seconds{1});
    FAIL() << "expected std::runtime_error";
  } catch (const std::runtime_error& e) {
    const std::string what = e.what();
    EXPECT_NE(what.find("myBaton"), std::string::npos) << what;
    EXPECT_NE(what.find("1s"), std::string::npos) << what;
    EXPECT_NE(what.find("timed out"), std::string::npos) << what;
  }
}

} // namespace
} // namespace facebook::bgp::test
