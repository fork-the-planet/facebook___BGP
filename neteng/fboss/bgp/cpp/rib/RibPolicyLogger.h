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
#include <cstdint>
#include <memory>
#include <string>

namespace facebook::bgp {

/**
 * Logs rib policy version transitions. Abstract so the logging backend is not
 * part of the public interface; the concrete implementation lives in the
 * build-specific source set and is built through createRibPolicyLogger(). OSS
 * builds have no implementation.
 */
class RibPolicyLogger {
 public:
  virtual ~RibPolicyLogger() = default;

  virtual size_t log(int64_t psPolicyVersion, int64_t rfPolicyVersion) = 0;
};

/**
 * Returns nullptr when rib policy logging is unavailable (e.g. OSS builds) or
 * disabled by flag.
 */
std::unique_ptr<RibPolicyLogger> createRibPolicyLogger(
    const std::string& deviceName);

} // namespace facebook::bgp
