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

#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"

namespace facebook::nettools::bgplib {

enum class BgpSessionState : uint8_t {
  IDLE = 1,
  ACTIVE,
  CONNECT,
  OPEN_SENT,
  OPEN_CONFIRM,
  ESTABLISHED
};

std::string getBgpSessionStateName(BgpSessionState state);

enum class ResetReason : uint8_t {
  OPEN_MSG_TIMER_EXPIRE = 0,
  HOLD_TIMER_EXPIRE,
  SOCKET_ERR,
  PARSE_ERR,
  SESSION_ERR,
  NOTIFICATION_RCVD,
  MANUAL_STOP
};

std::string getResetReasonName(ResetReason reason);

// All the information needed to be displayed
// To hide implementation details of FiberBgpPeer we are copying the data
// to new struct and returning rather than returning allPeers_ etc
// This should handle both static and dynamic peers
struct BgpPeerDisplayInfo {
  bgp::PeeringParams peeringParams;
  uint32_t remoteBgpId;
  std::optional<uint16_t> remoteGrRestartTime;
  BgpSessionState state;
  folly::SocketAddress localAddr;
  std::chrono::steady_clock::time_point startTime;
  std::chrono::steady_clock::time_point establishedTime;
  BgpCapabilities negotiatedCapabilities;
  std::optional<std::chrono::seconds> negotiatedHoldTime;
  uint32_t numOfConnectionAttempts;
  int64_t lastResetHoldTimer;
  int64_t lastResetKeepAliveTimer{0};
  int64_t lastReceivedKeepAlive;
  int64_t lastSentKeepAlive;
  uint32_t sendQueueBlocks{0};
  uint32_t totalSocketEgressBufferedEvents{0};
  uint64_t sendQueueTotalBlockDurationMs{0};
  uint64_t lastSendQueueBlockTimeMs{0};
  uint64_t lastSocketEgressBufferedTimeMs{0};
  std::optional<ResetReason> lastResetReason;
  std::chrono::steady_clock::time_point lastResetTime;
  int64_t numResets;
};

} // namespace facebook::nettools::bgplib
