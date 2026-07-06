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

/*
 * E2E tests: P-INIT x Block/Duration/N/A events
 *
 * Prefix range: 30.x.0.0/16
 * Fixtures: UpdateGroupMultiPeerTest, UpdateGroupSlowPeerDetectionTest
 *
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * P-INIT x E-SLOW-DUR
 * Duration timer fires during init dump. Single-peer with pre-loaded
 * routes. Block peer, set 1ms duration threshold AFTER bringUpPeer
 * (peer needs update group). Timer fires immediately -> detachment.
 *
 * Learned: setSlowPeerThresholds must be called AFTER bringUpPeer
 * because the peer has no update group until it's brought up.
 */
TEST_P(UpdateGroupSlowPeerDetectionTest, PeerInit_SlowDur) {
  XLOG(INFO, "=== TEST: PeerInit_SlowDur ===");

  addPeer(kDefaultPeerSpec3);
  addLocalRoute("30.5.0.0/16", {"3005:1"}, 100);
  addLocalRoute("30.6.0.0/16", {"3006:1"}, 100);
  addLocalRoute("30.7.0.0/16", {"3007:1"}, 100);
  setupSlowPeerComponents(3, 2, 0);

  BgpPeerId peerId3{kPeerAddr3, kPeerAddr3.asV4().toLongHBO()};

  /* Block peer3 BEFORE bringing up */
  blockPeer(kPeerAddr3);
  bringUpPeer(kPeerAddr3);

  /* Set aggressive duration threshold AFTER bringUpPeer.
   * Run on PeerManagerBase's EventBase to avoid TSAN race with
   * distributeMessageToInSyncPeers reading the config. */
  {
    auto group = getUpdateGroupForPeer(kPeerAddr3);
    ASSERT_NE(group, nullptr) << "Peer has no update group";
    UpdateGroupConfig cfg;
    cfg.allowSlowPeerDetach = true;
    cfg.slowPeerTimeThreshold = std::chrono::milliseconds(1);
    cfg.slowPeerBlockCountThreshold = 999999;
    cfg.slowPeerBlockCountWindow = std::chrono::milliseconds(60000);
    peerManager_->getEventBase().runInEventBaseThreadAndWait(
        [&group, &cfg]() { group->setUpdateGroupConfigForTesting(cfg); });
  }

  /* Wait for the queue to block */
  ASSERT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* 1ms duration threshold fires immediately, but single-peer groups
   * preserve the sole synced member (SP-187). Peer stays JOINED_BLOCKED
   * because it's the only member -- group won't detach it. */
  auto durState = getPeerState(kPeerAddr3);
  EXPECT_TRUE(
      durState == PeerUpdateState::JOINED_BLOCKED ||
      durState == PeerUpdateState::DETACHED_BLOCKED)
      << "Expected JOINED_BLOCKED (sole member) or DETACHED_BLOCKED, got "
      << static_cast<int>(durState);

  /* Unblock and bring peer down to clean up */
  unblockPeer(kPeerAddr3);
  drainPeerQueueCompletely(peerId3);
  bringDownPeer(kPeerAddr3);
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));

  XLOG(INFO, "=== TEST PASSED: PeerInit_SlowDur ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupSlowPeerDetectionTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
