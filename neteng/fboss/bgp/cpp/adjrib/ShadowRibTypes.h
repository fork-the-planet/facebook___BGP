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

#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>

#include "neteng/fboss/bgp/cpp/changeTracker/TrackableObject.h"
#include "neteng/fboss/bgp/cpp/common/RibMessage.h"

namespace facebook::bgp {

/*
 * Type alias for the shadow RIB entries map.
 * This is the canonical definition used across PeerManagerBase,
 * UpdateGroupManager, and AdjRibGroup to ensure type consistency.
 *
 * The map stores shadow RIB entries wrapped in TrackableObject for change
 * tracking via the changeListTracker publish/subscribe pattern.
 */
using ShadowRibEntriesMap = folly::F14NodeMap<
    folly::CIDRNetwork,
    std::unique_ptr<TrackableObject<ShadowRibEntry>>>;

} // namespace facebook::bgp
