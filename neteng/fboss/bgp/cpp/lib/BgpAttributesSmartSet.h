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

#include <optional>
#include <unordered_set>
#include "BgpAttributesWrapper.h"

namespace facebook {
namespace nettools {
namespace bgplib {

/**
 * A wrapper on top of unordered_set<BgpAttributesWrapper>. It provides the
 * collection of BgpAttributes. This data structure is shared by BGP Monitor,
 * BGP Collector and BMP Collector.
 */
class BgpAttributesSmartSet {
 public:
  BgpAttributesSmartSet();
  explicit BgpAttributesSmartSet(
      std::unordered_set<BgpAttributesWrapper> attrSet);

  /**
   * Add a BgpAttributesWrapper entry to the set.
   * Returns the pointer to the BgpAttributes in the set.
   */
  const BgpAttributesWrapper addEntry(const bgplib::BgpAttributes& attrRef);

  /**
   * Add a BgpAttributesWrapper entry to the set by shared_ptr.
   * Returns the pointer to the BgpAttributes in the set.
   */
  const BgpAttributesWrapper addEntry(
      const std::shared_ptr<bgplib::BgpAttributes> attrPtr);

  /**
   * Given a BgpAttributes, get the corresponding BgpAttributesWrapper in the
   * set, returns std::nullopt if no such entry.
   */
  std::optional<BgpAttributesWrapper> getEntry(
      const bgplib::BgpAttributes& attrRef);

  /**
   * Check if the BgpAttributes exisits in the set.
   */
  bool containsEntry(const bgplib::BgpAttributes& attrRef) const;

  /**
   * Get total number of unique stored attributes
   */
  int64_t size() const;

 private:
  std::unordered_set<BgpAttributesWrapper> attributesSet_;

  /**
   * Remove a BgpAttributesWrapper from the set.
   */
  void removeEntry(BgpAttributesWrapper& attrWrapper);

  /**
   * Clear the set.
   */
  void clear();

  friend class BgpAttributesWrapper;
};

} // namespace bgplib
} // namespace nettools
} // namespace facebook
