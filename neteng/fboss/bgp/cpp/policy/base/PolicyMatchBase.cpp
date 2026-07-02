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

#include "neteng/fboss/bgp/cpp/policy/base/PolicyMatchBase.h"

#include "neteng/fboss/bgp/cpp/policy/base/PolicyUtils.h"

namespace facebook {
namespace routing {

bool PrefixTree::Match(const folly::CIDRNetwork& prefix) const noexcept {
  facebook::network::RadixTree<
      folly::IPAddress,
      std::vector<PrefixLenOrRegexComp>>::VecConstIterators trail;
  auto matchItr = radixTree_
                      .longestMatchWithTrail(
                          prefix.first,
                          prefix.second,
                          trail,
                          false /* include non value
nodes*/);

  if (matchItr.atEnd()) {
    return false; // nothing matches
  }

  // get all the matches then sort it by its original order
  std::vector<IPWithRankedMatch> matches{};
  for (auto it = trail.begin(); it != trail.end(); it++) {
    const auto& compConditions = it->value();
    if (compConditions.empty()) {
      continue; // no conditions to match against
    }
    for (const auto& curComp : compConditions) {
      bool compRes = true;
      if (holds_alternative<PrefixLenRanges>(curComp)) {
        const auto& prefixLenRanges = get<PrefixLenRanges>(curComp);
        for (int i = 0; i < prefixLenRanges.size(); i++) {
          compRes &= CompareNumValue(
              prefixLenRanges[i].first,
              prefixLenRanges[i].second,
              prefix.second);
          if (!compRes) {
            // don't need to compare rest of logic if one does not match
            break;
          }
        }
      } else {
        compRes = re2::RE2::FullMatch(
            fmt::format(
                "{}/{}", prefix.first.toFullyQualified(), prefix.second),
            *get<std::shared_ptr<re2::RE2>>(curComp));
      }
      if (compRes) {
        if ((!needCheckOrder_) || origOrder_.empty()) {
          // Return if we don't need to check order or the origOrder_ is empty
          return true;
        }
        auto ip = it->ipAddress();
        auto masklen = it->masklen();
        auto item = folly::CIDRNetwork(ip, masklen);
        if (origOrder_.contains(item)) {
          RankedMatch rankedMatch = origOrder_.at(item);
          IPWithRankedMatch ipWithRankedMatch{
              .ip = item, .rankedMatch = rankedMatch};
          matches.emplace_back(std::move(ipWithRankedMatch));
        }
      }
    }
  }

  if (matches.empty()) {
    return false;
  }

  sort(
      matches.begin(),
      matches.end(),
      [](const IPWithRankedMatch& a, const IPWithRankedMatch& b) {
        return a.rankedMatch.order < b.rankedMatch.order;
      });

  return matches.at(0).rankedMatch.match;
}

} // namespace routing
} // namespace facebook
