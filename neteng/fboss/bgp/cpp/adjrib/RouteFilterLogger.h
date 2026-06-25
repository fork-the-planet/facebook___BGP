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

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include <folly/IPAddress.h>

namespace facebook::bgp {

/**
 * Logs the result of evaluating a route filter statement against a prefix.
 * Abstract so the logging backend is not part of the public interface; the
 * concrete implementation lives in the build-specific source set and is built
 * through RouteFilterLoggerFactory. OSS builds have no implementation.
 */
class RouteFilterLogger {
 public:
  virtual ~RouteFilterLogger() = default;

  virtual size_t log(
      bool egress,
      const folly::CIDRNetwork& prefix,
      bool allow,
      bool permissive,
      const std::vector<std::string>& communities) = 0;
};

/**
 * Builds RouteFilterLogger instances that share a single logging backend, so
 * constructing a logger per route filter statement does not allocate a new
 * backend each time.
 */
class RouteFilterLoggerFactory {
 public:
  virtual ~RouteFilterLoggerFactory() = default;

  virtual std::unique_ptr<RouteFilterLogger> create(
      const std::string& deviceName,
      const std::string& statementName,
      const std::string& peerName) const = 0;
};

// Returns nullptr when route filter logging is unavailable (e.g. OSS builds).
std::unique_ptr<RouteFilterLoggerFactory> createRouteFilterLoggerFactory();

} // namespace facebook::bgp
