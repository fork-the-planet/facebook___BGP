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

#include "neteng/fboss/bgp/cpp/lib/BgpAttributesSmartSet.h"

namespace facebook {
namespace nettools {
namespace bgplib {

BgpAttributesSmartSet::BgpAttributesSmartSet() {}

BgpAttributesSmartSet::BgpAttributesSmartSet(
    std::unordered_set<BgpAttributesWrapper> attrSet)
    : attributesSet_(attrSet) {}

const BgpAttributesWrapper BgpAttributesSmartSet::addEntry(
    const bgplib::BgpAttributes& attrRef) {
  auto attrWrapper = BgpAttributesWrapper(
      std::make_shared<bgplib::BgpAttributes>(attrRef), this);

  return *(attributesSet_.insert(attrWrapper).first);
}

const BgpAttributesWrapper BgpAttributesSmartSet::addEntry(
    const std::shared_ptr<bgplib::BgpAttributes> attrPtr) {
  auto attrWrapper = BgpAttributesWrapper(attrPtr, this);

  return *(attributesSet_.insert(attrWrapper).first);
}

void BgpAttributesSmartSet::removeEntry(BgpAttributesWrapper& attrWrapper) {
  attributesSet_.erase(attrWrapper);
}

std::optional<BgpAttributesWrapper> BgpAttributesSmartSet::getEntry(
    const bgplib::BgpAttributes& attrRef) {
  auto attrWrapper =
      BgpAttributesWrapper(std::make_shared<bgplib::BgpAttributes>(attrRef));
  auto result = attributesSet_.find(attrWrapper);

  if (result != attributesSet_.end()) {
    return *result;
  }

  return std::nullopt;
}

bool BgpAttributesSmartSet::containsEntry(
    const bgplib::BgpAttributes& attrRef) const {
  auto attrWrapper =
      BgpAttributesWrapper(std::make_shared<bgplib::BgpAttributes>(attrRef));
  return attributesSet_.find(attrWrapper) != attributesSet_.end();
}

void BgpAttributesSmartSet::clear() {
  attributesSet_.clear();
}

int64_t BgpAttributesSmartSet::size() const {
  return attributesSet_.size();
}

} // namespace bgplib
} // namespace nettools
} // namespace facebook
