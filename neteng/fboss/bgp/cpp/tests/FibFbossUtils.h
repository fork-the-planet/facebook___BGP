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

/**
 * @file FibFbossUtils.h
 * @brief Test fixture for testing FibFboss methods.
 * */
#pragma once

#include "neteng/fboss/bgp/cpp/tests/BoundedWaitUtils.h"
#include "neteng/fboss/bgp/cpp/tests/FibFbossMock.h"
namespace facebook::bgp {

/**
 * @class FibFbossUtilFixture
 * @brief Test fixture for testing FibFboss methods.
 *
 * This class provides a test fixture for testing FibFboss methods. It sets up a
 * mock FBOSS agent and a mock FIB.
 */
class FibFbossUtilFixture : public ::testing::Test {
 public:
  /**
   * @brief Setup the test fixture.
   *
   * Sets up the mock FBOSS agent and the mock FIB, and starts the event base
   * thread.
   */
  void SetUp() override {
    // Set up the mock agent client
    agentServiceHandler_ = std::make_shared<MockFbossCtrlSvIf>();
    agentServerThread_ =
        std::make_shared<apache::thrift::ScopedServerInterfaceThread>(
            agentServiceHandler_,
            "::1",
            0, // // Use a random available port and not
               // facebook::bgp::kFbossAgentPort
            services::TLSConfig::applyDefaultsToThriftServer);
    servicerouter::getMockSRClientFactory(false).registerMockService(
        "agent", agentServerThread_);

    evbThread_ = std::thread([&]() {
      XLOG(INFO, "Start evb for MockFib programming...");
      fib_ = FibFbossMock::createFibFboss(&evb_, asyncScope_, fibMsgQ_);
      fib_->agentAliveSince_ = kAliveSince_;
      evb_.loopForever();
      XLOG(INFO, "Successfully terminated evbThread");
    });
    evb_.waitUntilRunning();
  }

  /**
   * @brief Tear down the test fixture.
   *
   * Stops the event base thread, cancels any outstanding coroutines, and resets
   * the mock FBOSS agent and the mock FIB.
   */
  void TearDown() override {
    folly::coro::blockingWait(fib_->asyncScope_.cancelAndJoinAsync());
    evb_.runInEventBaseThreadAndWait([&]() { fib_->stop(); });

    agentServerThread_.reset();
    agentServiceHandler_.reset();
    servicerouter::getMockSRClientFactory(false).unregisterMockService("agent");

    fib_.reset();
    evb_.terminateLoopSoon();
    evbThread_.join();
  }

  /**
   * @brief Verifies that a full sync request has been received, with
   * appropriate delay.
   *
   * This coroutine waits for a message to be received on the fibMsgQ_ queue,
   * then checks that the message is a FibSyncReq and that at least
   * expectedDelay have passed since the start of the test.
   *
   * @param expectedDelay Expected delay to receive the full sync request.
   */
  folly::coro::Task<void> verifyDelayedFullSyncRequest(uint32_t expectedDelay) {
    // Record the start time of the test
    auto start = std::chrono::steady_clock::now();
    /*
     * Wait for a message to be received on the fibMsgQ_ queue. Bounded
     * so a never-arriving message fails the test fast instead of hanging.
     */
    auto msg = co_await facebook::bgp::test::boundedPop(fibMsgQ_, "fibMsgQ_");
    // Record the end time of the test
    auto end = std::chrono::steady_clock::now();
    // Calculate the duration of the test
    auto duration =
        std::chrono::duration_cast<std::chrono::seconds>(end - start);
    // Verify that at expectedDelay seconds have passed
    EXPECT_GE(duration.count(), expectedDelay);
    // Verify that the message is a FibSyncReq
    EXPECT_TRUE(std::holds_alternative<Fib::FibSyncReq>(msg));
  }

  /**
   * @brief Verifies that a full sync request has been received, immediately.
   *
   * This coroutine waits for a message to be received on the fibMsgQ_ queue,
   * then checks that the message is a FibSyncReq and that at least
   * expectedDelay have passed since the start of the test.
   */
  folly::coro::Task<void> verifyImmediateFullSyncRequest() {
    // Record the start time of the test
    auto start = std::chrono::steady_clock::now();
    /*
     * Ignore the first message, it is saying Fib is successfully
     * programmed. Bounded so a never-arriving message fails the test fast.
     */
    auto msg1 =
        co_await facebook::bgp::test::boundedPop(fibMsgQ_, "fibMsgQ_(msg1)");
    auto msg2 =
        co_await facebook::bgp::test::boundedPop(fibMsgQ_, "fibMsgQ_(msg2)");

    // Record the end time of the test
    auto end = std::chrono::steady_clock::now();
    // Calculate the duration of the test
    auto duration =
        std::chrono::duration_cast<std::chrono::seconds>(end - start);
    // Verify that at expectedDelay seconds have passed
    EXPECT_LE(duration.count(), fib_->holdDownState_.get()->kLowBackoffTicks);
    // Verify that the message is a FibSyncReq
    EXPECT_TRUE(std::holds_alternative<Fib::FibProgrammedMessage>(msg1));

    EXPECT_TRUE(std::holds_alternative<Fib::FibSyncReq>(msg2));
  }

  /**
   * @brief Coroutine to simulate a syncFib response.
   *
   */
  folly::coro::Task<void> returnSyncFibResp() {
    co_return;
  }

  std::shared_ptr<apache::thrift::ScopedServerInterfaceThread>
      agentServerThread_;
  std::shared_ptr<MockFbossCtrlSvIf> agentServiceHandler_;
  std::unique_ptr<FibFbossMock> fib_;
  folly::EventBase evb_;

  const uint32_t kAliveSince_ = 1;

  /**
   * @brief Asynchronous scope for coroutines.
   */
  folly::coro::CancellableAsyncScope asyncScope_;
  std::thread evbThread_;
  bgp::coro::MPMCQueue<Fib::FibMessage> fibMsgQ_;
  folly::F14NodeMap<folly::IPAddress, facebook::bgp::NexthopInfo>
      nexthopInfoMap_;
};

} // namespace facebook::bgp
