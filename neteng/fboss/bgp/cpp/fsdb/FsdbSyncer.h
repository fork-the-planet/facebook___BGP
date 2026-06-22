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

#include "fboss/fsdb/client/FsdbPubSubManager.h"
#include "fboss/fsdb/client/FsdbSyncManager.h"

#include "fboss/fsdb/if/FsdbModel.h"

namespace bgp_thrift = facebook::neteng::fboss::bgp::thrift;

DECLARE_bool(publish_rib_to_fsdb);

namespace facebook::bgp {

class FsdbSyncer {
 public:
  FsdbSyncer();

  void setConfig(const thrift::BgpConfig& config);
  void setRouteAttributePolicy(
      std::optional<rib_policy::TRouteAttributePolicy>&& routeAttributePolicy);

  void setPathSelectionPolicy(
      std::optional<rib_policy::TPathSelectionPolicy>&& pathSelectionPolicy);

  void setRouteFilterPolicy(
      std::optional<rib_policy::TRouteFilterPolicy>&& routeFilterPolicy);

  void setPartialDrainState(
      std::optional<bgp_thrift::TPartialDrainState>&& partialDrainState);

  // setRibMap() must be called before start() for initial sync of RibMap to
  // FSDB.
  void setRibMap(std::map<std::string, bgp_thrift::TRibEntry> rib);
  // updateRibMap() is called after start() to publish RIB updates to FSDB.
  void updateRibMap(
      std::map<std::string, std::optional<bgp_thrift::TRibEntry>> ribUpdate);

  void start();
  void stop();

 private:
  std::unique_ptr<fboss::fsdb::FsdbPubSubManager> fsdbPubSubMgr_;
  std::unique_ptr<fboss::fsdb::FsdbSyncManager<
      fboss::fsdb::BgpData,
      true /* EnablePatchAPIs */>>
      stateSyncer_;
};

} // namespace facebook::bgp
