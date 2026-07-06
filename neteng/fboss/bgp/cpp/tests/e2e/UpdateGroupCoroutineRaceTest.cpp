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
 * [BGP++][UG2 E2E] Coroutine race condition tests.
 *
 * Tests for Bug 5 (P2260468424): SIGSEGV use-after-free during backpressure.
 * The buildAndSendGroupBgpMessages() coroutine suspends at co_await, and
 * concurrent session flaps can clear the attrToPrefixMap_, invalidating
 * iterators the coroutine holds.
 *
 * These tests verify no crashes when: (1) cause suspension via backpressure,
 * (2) inject concurrent peer DOWN events, (3) resume coroutine.
 *
 * Learning patterns applied:
 *   P6: Inject >= queue capacity (3) routes for reliable blocking
 *   P8: Use 3 peers
 *   P9: Always waitForPeerState(DOWN) after bringDownPeer()
 */

#include "neteng/fboss/bgp/cpp/tests/e2e/UpdateGroupSlowPeerTestCommon.h"

using namespace facebook::nettools::bgplib;

namespace facebook {
namespace bgp {

/*
 * Test 2 (Critical): GWaiting_SessionFlapDuringBackpressuredPLDrain
 *
 * Coroutine suspended on backpressure, then all OTHER in-sync peers
 * go down simultaneously (triggers clearPackingList). Coroutine resumes
 * with stale iterators — must not crash.
 *
 * Flow:
 * 1. 3 peers joined
 * 2. Block peer3's queue (backpressure)
 * 3. Inject routes with distinct communities -> attrToPrefixMap_ built
 * 4. Drain peer4/peer5 to confirm receipt
 * 5. Inject more routes while blocked -> coroutine suspends
 * 6. Bring down peer4 AND peer5 (concurrent mutation)
 * 7. Unblock peer3 -> coroutine resumes
 * 8. Assert: no crash
 */
TEST_P(
    UpdateGroupCoroutineRaceTest,
    GWaiting_SessionFlapDuringBackpressuredPLDrain) {
  XLOGF(INFO, "=== TEST: GWaiting_SessionFlapDuringBackpressuredPLDrain ===");

  auto [peerId3, peerId4, peerId5] = setupThreePeersJoined();

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Block peer3 */
  blockPeer(kPeerAddr3);

  /* Inject 3 routes with distinct communities to build attrToPrefixMap_ (P6) */
  injectLocalRoutesAtRuntime({"90.0.0.0/8"}, {"900:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("90.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "90.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "900:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "90.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "900:1"));

  injectLocalRoutesAtRuntime({"91.0.0.0/8"}, {"910:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("91.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "91.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "910:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "91.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "910:1"));

  injectLocalRoutesAtRuntime({"92.0.0.0/8"}, {"920:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("92.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "92.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "920:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "92.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "920:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Inject more routes while peer3 blocked — deepens the PL */
  injectLocalRoutesAtRuntime({"93.0.0.0/8"}, {"930:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("93.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "93.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "930:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "93.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "930:1"));

  /*
   * CONCURRENT MUTATION: Bring down peer4 and peer5 while coroutine
   * is suspended on peer3's backpressure. This may trigger
   * clearPackingList() on the group, invalidating iterators.
   */
  bringDownPeer(kPeerAddr4);
  bringDownPeer(kPeerAddr5);

  /* P9: Wait for both peers to be fully DOWN */
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Resume the coroutine by unblocking peer3 */
  unblockPeer(kPeerAddr3);

  /*
   * The critical assertion: process did not crash (SIGSEGV).
   * If we reach this line, the coroutine handled the stale
   * iterators safely.
   */
  XLOGF(INFO, "Process alive after concurrent mutation — no crash");

  /* Peer3 may be in various valid states — just verify it's not stuck */
  auto state3 = getPeerState(kPeerAddr3);
  EXPECT_NE(state3, PeerUpdateState::INIT)
      << "Peer3 should not be stuck in INIT";

  XLOGF(
      INFO,
      "=== TEST PASSED: GWaiting_SessionFlapDuringBackpressuredPLDrain ===");
}

/*
 * Test 9 (High): GWaiting_GroupDestroyDuringBackpressuredPLDrain
 *
 * All 3 peers go down while group is in WAITING state with active PL.
 * This triggers maybeDestroyUpdateGroup() -> clearPackingList() while
 * the coroutine may still be referencing the PL.
 */
TEST_P(
    UpdateGroupCoroutineRaceTest,
    GWaiting_GroupDestroyDuringBackpressuredPLDrain) {
  XLOGF(INFO, "=== TEST: GWaiting_GroupDestroyDuringBackpressuredPLDrain ===");

  auto [peerId3, peerId4, peerId5] = setupThreePeersJoined();

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Block peer3, inject routes */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"95.0.0.0/8"}, {"950:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("95.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "950:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "95.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "950:1"));

  injectLocalRoutesAtRuntime({"96.0.0.0/8"}, {"960:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("96.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "960:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "96.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "960:1"));

  injectLocalRoutesAtRuntime({"97.0.0.0/8"}, {"970:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("97.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "97.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "970:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "97.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "970:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Bring down ALL 3 peers — group should be destroyed */
  bringDownPeer(kPeerAddr3);
  bringDownPeer(kPeerAddr4);
  bringDownPeer(kPeerAddr5);

  /* P9: Wait for all DOWN */
  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::DOWN));

  /* Process alive — no crash */
  XLOGF(INFO, "All peers down, group destroyed safely — no crash");

  XLOGF(
      INFO,
      "=== TEST PASSED: GWaiting_GroupDestroyDuringBackpressuredPLDrain ===");
}

/*
 * Test 15 (Medium): GWaiting_BackpressureThenSinglePeerFlap
 *
 * One in-sync peer flaps while another is backpressured.
 * Less severe than Test 2 (only 1 peer goes down, not all).
 */
TEST_P(UpdateGroupCoroutineRaceTest, GWaiting_BackpressureThenSinglePeerFlap) {
  XLOGF(INFO, "=== TEST: GWaiting_BackpressureThenSinglePeerFlap ===");

  auto [peerId3, peerId4, peerId5] = setupThreePeersJoined();

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Block peer3, inject routes */
  blockPeer(kPeerAddr3);

  injectLocalRoutesAtRuntime({"98.0.0.0/8"}, {"980:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("98.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "98.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "980:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "98.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "980:1"));

  injectLocalRoutesAtRuntime({"99.0.0.0/8"}, {"990:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("99.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "99.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "990:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "99.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "990:1"));

  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /* Single peer flap while peer3 is backpressured */
  bringDownPeer(kPeerAddr4);
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::DOWN));

  /* Unblock peer3 — coroutine resumes with one fewer in-sync peer */
  unblockPeer(kPeerAddr3);

  /* No crash — verify peer3 and peer5 are in valid states */
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Verify peer5 can still receive new routes */
  injectLocalRoutesAtRuntime({"100.0.0.0/8"}, {"1000:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("100.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "100.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1000:1"));

  XLOGF(INFO, "=== TEST PASSED: GWaiting_BackpressureThenSinglePeerFlap ===");
}

/*
 * Test: GWaiting_MapClearedWhileBackpressuredOnErase
 *
 * Validates the fix that moves attrToPrefixMap_.erase(it) above
 * co_await distributeMessageToInSyncPeers(). Without the fix, the
 * coroutine suspends holding a stale iterator after buildGroupUpdate()
 * drains a prefix set, and a concurrent clearPackingList() invalidates
 * it — causing SIGSEGV on resume.
 *
 * Flow:
 * 1. 3 peers joined, block peer3
 * 2. Inject routes so the packing list has entries to iterate
 * 3. Wait for peer3 queue to block (coroutine suspended mid-PL)
 * 4. Directly clear the packing list on the PeerManagerBase EVB
 * 5. Unblock peer3 — coroutine resumes
 * 6. Assert: no crash (iterator was consumed before co_await)
 */
TEST_P(
    UpdateGroupCoroutineRaceTest,
    GWaiting_MapClearedWhileBackpressuredOnErase) {
  XLOGF(INFO, "=== TEST: GWaiting_MapClearedWhileBackpressuredOnErase ===");

  auto [peerId3, peerId4, peerId5] = setupThreePeersJoined();

  ASSERT_TRUE(waitForPeerState(kPeerAddr3, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr4, PeerUpdateState::JOINED_RUNNING));
  ASSERT_TRUE(waitForPeerState(kPeerAddr5, PeerUpdateState::JOINED_RUNNING));

  /* Block peer3's egress queue */
  blockPeer(kPeerAddr3);

  /*
   * Inject 3 routes with distinct communities. Each produces a separate
   * BGP UPDATE (different attrs). buildGroupUpdate() will fully drain
   * each entry's prefix set (pfxSet.empty() == true), triggering the
   * erase(it) path. With the fix, erase happens before co_await.
   */
  injectLocalRoutesAtRuntime({"101.0.0.0/8"}, {"1010:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("101.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "101.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1010:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "101.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1010:1"));

  injectLocalRoutesAtRuntime({"102.0.0.0/8"}, {"1020:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("102.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "102.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1020:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "102.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1020:1"));

  injectLocalRoutesAtRuntime({"103.0.0.0/8"}, {"1030:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("103.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "103.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1030:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "103.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1030:1"));

  /* Confirm peer3's queue is blocked — coroutine is suspended */
  EXPECT_TRUE(waitForPeerQueueBlocked(peerId3));

  /*
   * Directly clear the packing list while the coroutine is suspended
   * on peer3's backpressure. This invalidates any iterators the
   * coroutine holds into attrToPrefixMap_.
   */
  auto group = getUpdateGroupForPeer(kPeerAddr3);
  ASSERT_NE(group, nullptr);
  peerManager_->getEventBase().runInEventBaseThreadAndWait(
      [&group]() { group->clearPackingList(); });

  /* Unblock peer3 — coroutine resumes with cleared map */
  unblockPeer(kPeerAddr3);

  /* If we reach here, the fix works — no SIGSEGV from stale iterator */
  XLOGF(
      INFO,
      "Map cleared during backpressure, coroutine resumed safely — no crash");

  /* Verify peers are still functional */
  injectLocalRoutesAtRuntime({"104.0.0.0/8"}, {"1040:1"}, 150);
  ASSERT_TRUE(
      waitForRouteInShadowRib(folly::IPAddress::createNetwork("104.0.0.0/8")));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "104.0.0.0",
      8,
      kPeerAddr4,
      getExpectedNexthop(kPeerAddr4),
      "4200000001",
      "1040:1"));
  EXPECT_TRUE(verifyRouteAdd(
      "v4",
      "104.0.0.0",
      8,
      kPeerAddr5,
      getExpectedNexthop(kPeerAddr5),
      "4200000001",
      "1040:1"));

  XLOGF(
      INFO,
      "=== TEST PASSED: GWaiting_MapClearedWhileBackpressuredOnErase ===");
}

INSTANTIATE_TEST_SUITE_P(
    SerializationModes,
    UpdateGroupCoroutineRaceTest,
    ::testing::Values(kNoSerialization, kWithSerialization),
    [](const ::testing::TestParamInfo<SerializationParams>& info) {
      return info.param.name;
    });

} // namespace bgp
} // namespace facebook
