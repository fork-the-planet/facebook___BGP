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

#include "neteng/fboss/bgp/cpp/peer/PeerManagerBase.h"
#include "neteng/fboss/bgp/if/gen-cpp2/TBgpServiceStream.h"

namespace facebook::bgp {

class BgpServiceStream
    : public facebook::neteng::fboss::bgp::thrift::TBgpServiceStreamSvIf {
 public:
  explicit BgpServiceStream(PeerManagerBase* peerMgr) : peerMgr_(peerMgr) {}
  ~BgpServiceStream() override = default;

  // Stream API
  apache::thrift::ServerStream<neteng::fboss::bgp::thrift::TBgpRouteDelta>
  subscribe(std::unique_ptr<std::string> subscriberName) override;

 private:
  PeerManagerBase* peerMgr_{nullptr};
};
} // namespace facebook::bgp
