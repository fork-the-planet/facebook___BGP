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

#include <string>

#include <boost/filesystem.hpp>
#include <folly/Expected.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/logging/xlog.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

namespace facebook::bgp {

/*
 * Why a read failed, returned on the error side of
 * folly::Expected<T, ArtifactReadError> so callers can tell a benign absent
 * artifact apart from a genuine read/parse error. A plain std::optional cannot:
 * it collapses both into nullopt.
 */
enum class ArtifactReadError {
  kAbsent, // no path configured, or file not present (expected pre-onboarding)
  kError, // file present but could not be read or deserialized
};

template <typename T>
folly::Expected<T, ArtifactReadError> readThriftArtifactFromFile(
    const std::string& filePath) {
  if (filePath.empty()) {
    return folly::makeUnexpected(ArtifactReadError::kAbsent);
  }

  boost::system::error_code ec;
  if (!boost::filesystem::exists(filePath, ec)) {
    if (ec && ec != boost::system::errc::no_such_file_or_directory) {
      /*
       * exists() failed for a reason other than the file simply not being
       * there (e.g. permission denied on a configured path). The file may be
       * present but we cannot confirm it, so treat this as a real error rather
       * than a benign absence. A plain "not found" falls through to kAbsent.
       */
      XLOGF(ERR, "Filesystem error checking '{}': {}", filePath, ec.message());
      return folly::makeUnexpected(ArtifactReadError::kError);
    }
    XLOGF(INFO, "Artifact file not found: '{}'", filePath);
    return folly::makeUnexpected(ArtifactReadError::kAbsent);
  }

  std::string contents;
  if (!folly::readFile(filePath.c_str(), contents)) {
    XLOGF(
        ERR,
        "Failed to read artifact file '{}'. Error ({}): {}",
        filePath,
        errno,
        folly::errnoStr(errno));
    return folly::makeUnexpected(ArtifactReadError::kError);
  }

  try {
    return apache::thrift::SimpleJSONSerializer::deserialize<T>(contents);
  } catch (const std::exception& ex) {
    XLOGF(
        ERR,
        "Failed to deserialize artifact from '{}': {}",
        filePath,
        folly::exceptionStr(ex));
    return folly::makeUnexpected(ArtifactReadError::kError);
  }
}

} // namespace facebook::bgp
