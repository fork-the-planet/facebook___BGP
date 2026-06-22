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

#include "neteng/fboss/bgp/cpp/fsdb/FsdbSyncer.h"
#include <gflags/gflags.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>
#include <optional>

DEFINE_bool(
    publish_rib_to_fsdb,
    false,
    "Enable publishing RIB entries to FSDB");

namespace {
const thriftpath::RootThriftPath<facebook::fboss::fsdb::FsdbOperStateRoot>
    fsdbStateRootPath;
const auto bgpPath = fsdbStateRootPath.bgp();
const auto configPath = bgpPath.config();
const auto kPublisherId = "bgpd";

namespace k_fsdb_model = apache::thrift::ident;
} // namespace

namespace facebook::bgp {

FsdbSyncer::FsdbSyncer()
    : fsdbPubSubMgr_(
          std::make_unique<fboss::fsdb::FsdbPubSubManager>(kPublisherId)),
      stateSyncer_(
          std::make_unique<fboss::fsdb::FsdbSyncManager<
              fboss::fsdb::BgpData,
              true /* EnablePatchAPIs */>>(
              kPublisherId,
              bgpPath.tokens(),
              false /* isStats */,
              fboss::fsdb::getFsdbStatePubType())) {}

void FsdbSyncer::start() {
  stateSyncer_->start();
}

void FsdbSyncer::stop() {
  stateSyncer_->stop();
  stateSyncer_.reset();
  fsdbPubSubMgr_.reset();
}

void FsdbSyncer::setConfig(const thrift::BgpConfig& config) {
  stateSyncer_->updateState([config](const auto& oldState) {
    auto newState = oldState->clone();
    newState->template modify<k_fsdb_model::config>();
    newState->template ref<k_fsdb_model::config>()->fromThrift(config);
    return newState;
  });
}

void FsdbSyncer::setRouteAttributePolicy(
    std::optional<rib_policy::TRouteAttributePolicy>&& routeAttributePolicy) {
  stateSyncer_->updateState([routeAttributePolicy =
                                 std::move(routeAttributePolicy)](
                                const auto& oldState) mutable {
    auto newState = oldState->clone();
    newState->template modify<k_fsdb_model::routeAttributePolicy>();
    if (routeAttributePolicy) {
      newState->template ref<k_fsdb_model::routeAttributePolicy>()->fromThrift(
          std::move(*routeAttributePolicy));
    } else {
      newState->template ref<k_fsdb_model::routeAttributePolicy>() = nullptr;
    }
    return newState;
  });
}

void FsdbSyncer::setPathSelectionPolicy(
    std::optional<rib_policy::TPathSelectionPolicy>&& pathSelectionPolicy) {
  stateSyncer_->updateState([pathSelectionPolicy =
                                 std::move(pathSelectionPolicy)](
                                const auto& oldState) mutable {
    auto newState = oldState->clone();
    newState->template modify<k_fsdb_model::pathSelectionPolicy>();
    if (pathSelectionPolicy) {
      newState->template ref<k_fsdb_model::pathSelectionPolicy>()->fromThrift(
          std::move(*pathSelectionPolicy));
    } else {
      newState->template ref<k_fsdb_model::pathSelectionPolicy>() = nullptr;
    }
    return newState;
  });
}

void FsdbSyncer::setRouteFilterPolicy(
    std::optional<rib_policy::TRouteFilterPolicy>&& routeFilterPolicy) {
  stateSyncer_->updateState([routeFilterPolicy = std::move(routeFilterPolicy)](
                                const auto& oldState) mutable {
    auto newState = oldState->clone();
    newState->template modify<k_fsdb_model::routeFilterPolicy>();
    if (routeFilterPolicy) {
      newState->template ref<k_fsdb_model::routeFilterPolicy>()->fromThrift(
          std::move(*routeFilterPolicy));
    } else {
      newState->template ref<k_fsdb_model::routeFilterPolicy>() = nullptr;
    }
    return newState;
  });
}

void FsdbSyncer::setPartialDrainState(
    std::optional<bgp_thrift::TPartialDrainState>&& partialDrainState) {
  stateSyncer_->updateState([partialDrainState = std::move(partialDrainState)](
                                const auto& oldState) mutable {
    auto newState = oldState->clone();
    newState->template modify<k_fsdb_model::partialDrainState>();
    if (partialDrainState) {
      newState->template ref<k_fsdb_model::partialDrainState>()->fromThrift(
          std::move(*partialDrainState));
    } else {
      newState->template ref<k_fsdb_model::partialDrainState>() = nullptr;
    }
    return newState;
  });
}

void FsdbSyncer::setRibMap(std::map<std::string, bgp_thrift::TRibEntry> rib) {
  if (!FLAGS_publish_rib_to_fsdb) {
    return;
  }
  stateSyncer_->updateState([rib =
                                 std::move(rib)](const auto& oldState) mutable {
    auto newState = oldState->clone();
    newState->template modify<k_fsdb_model::ribMap>();
    newState->template ref<k_fsdb_model::ribMap>()->fromThrift(std::move(rib));
    return std::move(newState);
  });
}

void FsdbSyncer::updateRibMap(
    std::map<std::string, std::optional<bgp_thrift::TRibEntry>> ribUpdate) {
  if (!FLAGS_publish_rib_to_fsdb) {
    return;
  }
  stateSyncer_->updateState(
      [ribUpdates = std::move(ribUpdate)](const auto& oldState) mutable {
        auto newState = oldState->clone();
        newState->template modify<k_fsdb_model::ribMap>();
        auto& ribMap = newState->template ref<k_fsdb_model::ribMap>();
        for (auto& [prefix, ribEntry] : ribUpdates) {
          if (ribEntry.has_value()) {
            ribMap->modify(prefix);
            ribMap->ref(prefix)->fromThrift(std::move(ribEntry.value()));
          } else {
            ribMap->remove(prefix);
          }
        }
        return std::move(newState);
      });
}

} // namespace facebook::bgp
