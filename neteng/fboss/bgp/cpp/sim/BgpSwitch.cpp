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

#include "neteng/fboss/bgp/cpp/sim/BgpSwitch.h"

#include <algorithm>
#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <fmt/format.h>
#include <folly/IPAddress.h>
#include <folly/container/F14Map.h>
#include <folly/container/F14Set.h>
#include <folly/logging/xlog.h>

#include "neteng/fboss/bgp/cpp/adjrib/AdjRibCommon.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/common/Consts.h"
#include "neteng/fboss/bgp/cpp/config/ConfigStructs.h"
#include "neteng/fboss/bgp/cpp/config/ConfigUtils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyManager.h"
#include "neteng/fboss/bgp/cpp/policy/PolicyStructs.h"
#include "neteng/fboss/bgp/cpp/sim/SimRouteInfo.h"

namespace facebook::bgp {

namespace {

/*
 * Parse the config router-id string into a folly::IPAddress, defaulting to
 * 0.0.0.0 when unset/unparseable (matches the simulator's defensive parsing).
 */
folly::IPAddress routerIdToIp(const thrift::BgpConfig& config) {
  auto maybeIp = folly::IPAddress::tryFromString(*config.router_id());
  if (maybeIp.hasValue()) {
    return maybeIp.value();
  }
  return folly::IPAddress("0.0.0.0");
}

/*
 * Numeric router-id used for best-path tie-breaking. BGP router-ids are 4-byte
 * (IPv4) values; non-v4/unset ids resolve to 0.
 */
uint32_t extractRouterId(const thrift::BgpConfig& config) {
  const auto ip = routerIdToIp(config);
  return ip.isV4() ? ip.asV4().toLongHBO() : 0;
}

// Local ASN with 4-byte preferred over the deprecated 2-byte field.
uint32_t extractLocalAsn(const thrift::BgpConfig& config) {
  if (config.local_as_4_byte().has_value()) {
    return static_cast<uint32_t>(*config.local_as_4_byte());
  }
  if (config.local_as().has_value()) {
    return static_cast<uint32_t>(*config.local_as());
  }
  return 0;
}

// Confederation ASN (if any), 4-byte preferred over the 2-byte field.
std::optional<uint32_t> extractLocalConfedAsn(const thrift::BgpConfig& config) {
  if (config.local_confed_as_4_byte().has_value()) {
    return static_cast<uint32_t>(*config.local_confed_as_4_byte());
  }
  if (config.local_confed_as().has_value()) {
    return static_cast<uint32_t>(*config.local_confed_as());
  }
  return std::nullopt;
}

/*
 * Derive the RoutingTable's config from global config, mirroring the bestpath
 * feature flags production reads from BgpSettingConfig.
 */
RoutingTableConfig makeRoutingTableConfig(
    const thrift::BgpConfig& config,
    uint32_t routerId,
    uint32_t localAsn,
    const std::optional<uint32_t>& localConfedAsn) {
  RoutingTableConfig rtConfig;
  rtConfig.routerId = routerId;
  rtConfig.localAs4Byte = localAsn;
  rtConfig.localConfedAs4Byte = localConfedAsn.value_or(0);

  if (const auto setting = config.bgp_setting_config()) {
    rtConfig.enableMedComparison =
        setting->enable_med_comparison().value_or(false);
    rtConfig.enableMedMissingAsWorst =
        setting->enable_med_missing_as_worst().value_or(false);
    rtConfig.enableWeightComparison =
        setting->enable_weight_comparison().value_or(false);
    rtConfig.enableEiBgpMultipath =
        setting->enable_eibgp_multipath().value_or(false);
  }
  rtConfig.countConfedsInAsPathLen =
      config.count_confeds_in_as_path_len().value_or(false);
  return rtConfig;
}

/*
 * Build the BgpPath for a locally originated network, mirroring
 * RibBase::createLocalRoute(): default nexthop, IGP origin, default local-pref,
 * optional communities/as-path, and the local-route weight applied up front so
 * an origination policy can observe and override it. Returns nullptr (and logs)
 * when the configured origin is outside the valid BgpAttrOrigin range, matching
 * the production validation. The returned path is unpublished.
 */
std::shared_ptr<BgpPath> buildOriginatedPath(
    const folly::CIDRNetwork& prefix,
    const thrift::BgpNetwork& network) {
  nettools::bgplib::BgpPathC pathC;
  if (prefix.first.isV4()) {
    pathC.nexthop = network.nexthop().has_value()
        ? folly::IPAddress(*network.nexthop())
        : kLocalRouteV4Nexthop;
  } else {
    pathC.nexthop = network.nexthop().has_value()
        ? folly::IPAddress(*network.nexthop())
        : kLocalRouteV6Nexthop;
  }

  nettools::bgplib::BgpAttributesC attrs;
  if (network.origin().has_value()) {
    const auto origin = *network.origin();
    if (origin < static_cast<int>(apache::thrift::TEnumTraits<
                                  nettools::bgplib::BgpAttrOrigin>::min()) ||
        origin > static_cast<int>(apache::thrift::TEnumTraits<
                                  nettools::bgplib::BgpAttrOrigin>::max())) {
      XLOGF(
          ERR,
          "Invalid origin value {} for originated network {}",
          origin,
          *network.prefix());
      return nullptr;
    }
    attrs.origin = static_cast<nettools::bgplib::BgpAttrOrigin>(origin);
  } else {
    attrs.origin = nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP;
  }
  attrs.localPref = network.local_pref().has_value()
      ? static_cast<uint32_t>(*network.local_pref())
      : kDefaultLocalPref;
  if (network.communities().has_value()) {
    attrs.communities = createBgpAttrCommunitiesC(*network.communities());
  }
  if (network.as_path().has_value()) {
    attrs.asPath = createBgpAttrAsPathDedup(*network.as_path());
  }
  pathC.attrs = std::move(attrs);

  auto path = std::make_shared<BgpPath>(static_cast<BgpPathFields>(pathC));
  /*
   * Set the local-route weight before policy application so an origination
   * policy can observe and override it (matches RibBase::createLocalRoute,
   * where the weight is set on attrs before getBgpPathFromPolicy()).
   */
  path->setWeight(kLocalRouteWeight);
  return path;
}

// Map a peer-session classification to the RIB's route-origin enum.
RouteOrigin toRouteOrigin(PeerType type) {
  switch (type) {
    case PeerType::INTERNAL:
      return RouteOrigin::INTERNAL;
    case PeerType::EXTERNAL:
      return RouteOrigin::EXTERNAL;
    case PeerType::CONFED_EXTERNAL:
      return RouteOrigin::CONFED_EXTERNAL;
  }
  return RouteOrigin::EXTERNAL;
}

/*
 * The ASN that makes the path a loop for this switch, or std::nullopt when the
 * AS-path is loop-free. Mirrors production AdjRib::hasAsPathLoop
 * (adjrib/AdjRibUtil.cpp), which checks both the switch global ASN and the
 * per-peer effective local-AS against the regular AS segments:
 *  - globalAsn (the confederation identifier when this switch is a
 *    confederation member) and peerLocalAs (the per-peer local-AS, which
 *    includes the RFC 7705 local-as override) loop only if they appear in a
 *    regular AS segment (AS_SEQUENCE / AS_SET);
 *  - the confederation sub-AS (local_confed_as) loops only if it appears in a
 *    confederation segment (AS_CONFED_SEQUENCE / AS_CONFED_SET), and is checked
 *    only when this switch is a confederation member (localConfedAsn set).
 * Checking peerLocalAs in addition to globalAsn matches production: a route
 * carrying a peer's overridden local-AS is a loop even though it never carries
 * the switch global ASN. Matching each ASN against just its applicable segment
 * type avoids the false positives a blanket hasAsn() scan produces. Each ASN
 * is guarded against 0 (unset); when peerLocalAs == globalAsn the second check
 * is a harmless redundant scan.
 *
 * Returning the matched ASN (rather than a bare bool) lets callers log the
 * actual culprit: with a per-peer local-AS override the loop is triggered by
 * peerLocalAs, not globalAsn, so reporting globalAsn would mislead debugging.
 */
std::optional<uint32_t> asPathLoopAsn(
    const BgpPath& path,
    uint32_t globalAsn,
    uint32_t peerLocalAs,
    const std::optional<uint32_t>& localConfedAsn) {
  const auto inRegularSegment = [](const auto& seg, uint32_t asn) {
    return std::find(seg.asSequence.begin(), seg.asSequence.end(), asn) !=
        seg.asSequence.end() ||
        seg.asSet.count(asn) != 0;
  };
  for (const auto& seg : path.getAsPath().get()) {
    if (globalAsn != 0 && inRegularSegment(seg, globalAsn)) {
      return globalAsn;
    }
    if (peerLocalAs != 0 && inRegularSegment(seg, peerLocalAs)) {
      return peerLocalAs;
    }
    if (localConfedAsn.has_value() &&
        (std::find(
             seg.asConfedSequence.begin(),
             seg.asConfedSequence.end(),
             *localConfedAsn) != seg.asConfedSequence.end() ||
         seg.asConfedSet.count(*localConfedAsn) != 0)) {
      return *localConfedAsn;
    }
  }
  return std::nullopt;
}

/*
 * Apply outbound AS-path handling before advertising to a peer of type
 * `egressType`, mirroring production
 * AdjRibCommon::updateAsPathAttributesCommon:
 *  - INTERNAL (iBGP): the AS-path is left unchanged.
 *  - EXTERNAL (eBGP): drop confederation segments, then prepend the local ASN
 *    (the confederation identifier for a confed member) to the AS_SEQUENCE.
 *  - CONFED_EXTERNAL (confed-eBGP): prepend the confederation sub-AS
 *    (local_confed_as) to the AS_CONFED_SEQUENCE.
 * The input path is immutable (published); a clone is modified and republished
 * so the RIB-held best path is never mutated. Each peer gets its own clone.
 *
 * TODO: replaceZerosInAsPath, local-pref strip, MED unset
 */
std::shared_ptr<const BgpPath> applyEgressAsPath(
    const std::shared_ptr<const BgpPath>& path,
    PeerType egressType,
    uint32_t localAsn,
    const std::optional<uint32_t>& localConfedAsn) {
  if (egressType == PeerType::INTERNAL) {
    return path;
  }
  auto mutablePath = std::const_pointer_cast<BgpPath>(path)->clone();
  nettools::bgplib::BgpAttrAsPathC newAsPath = mutablePath->getAsPath().get();
  if (egressType == PeerType::CONFED_EXTERNAL) {
    prependAsPath(
        newAsPath, localConfedAsn.value_or(localAsn), /*isConfedPeer=*/true);
  } else {
    removeConfedAsPathSegments(newAsPath);
    prependAsPath(newAsPath, localAsn, /*isConfedPeer=*/false);
  }
  mutablePath->setAsPath(std::move(newAsPath));
  mutablePath->publish();
  return mutablePath;
}

} // namespace

BgpSwitch::BgpSwitch(std::string name, thrift::BgpConfig config)
    : name_(std::move(name)),
      routerId_(extractRouterId(config)),
      localAsn_(extractLocalAsn(config)),
      localConfedAsn_(extractLocalConfedAsn(config)),
      holdTime_(static_cast<uint32_t>(*config.hold_time())),
      networks4_(*config.networks4()),
      networks6_(*config.networks6()),
      routingTable_(makeRoutingTableConfig(
          config,
          static_cast<uint32_t>(routerId_),
          localAsn_,
          localConfedAsn_)) {
  /*
   * A BgpSwitch requires a valid local ASN; AS 0 is reserved/invalid
   * (RFC 7607). Reject it rather than silently propagating the sentinel to
   * every peer and the routing table.
   */
  if (localAsn_ == 0) {
    throw std::runtime_error(
        fmt::format(
            "BgpSwitch {}: missing or invalid local ASN (0); a valid local ASN is required",
            name_));
  }
  buildPeers(config);
  initPolicyManager(config);
}

BgpSwitch::~BgpSwitch() = default;

void BgpSwitch::buildPeers(const thrift::BgpConfig& config) {
  folly::F14FastMap<std::string, const thrift::PeerGroup*> groupsByName;
  if (config.peer_groups().has_value()) {
    for (const auto& group : *config.peer_groups()) {
      groupsByName.emplace(*group.name(), &group);
    }
  }

  peers_.reserve(config.peers()->size());
  for (const auto& peerConfig : *config.peers()) {
    const thrift::PeerGroup* group = nullptr;
    if (peerConfig.peer_group_name().has_value()) {
      const auto it = groupsByName.find(*peerConfig.peer_group_name());
      if (it == groupsByName.end()) {
        throw std::runtime_error(
            fmt::format(
                "BgpSwitch {}: peer {} references undefined peer group '{}'",
                name_,
                *peerConfig.peer_addr(),
                *peerConfig.peer_group_name()));
      }
      group = it->second;
    }
    auto& peer = peers_.emplace_back(peerConfig, group);
    peer.setLocalAsn(localAsn_);
    peer.setRouterId(routerId_);
  }
}

void BgpSwitch::initPolicyManager(const thrift::BgpConfig& config) {
  /*
   * Mirror Config::createPolicyManager(): a PolicyManager exists iff policies
   * are configured.
   */
  if (!config.policies().has_value()) {
    return;
  }

  /*
   * PolicyManager only reads the global config during construction (it does not
   * retain the pointer), so a stack-local instance is sufficient.
   */
  const auto routerIp = routerIdToIp(config);
  const BgpGlobalConfig globalConfig(
      localAsn_,
      routerIp, /* routerId */
      routerIp, /* clusterId */
      std::chrono::seconds(holdTime_),
      std::nullopt, /* listenAddr */
      std::nullopt, /* grRestartTime */
      {}, /* networksV4 */
      {}, /* networksV6 */
      localConfedAsn_);
  policyManager_ =
      std::make_unique<PolicyManager>(*config.policies(), &globalConfig);
}

void BgpSwitch::originateRoutes() {
  if (routesOriginated_) {
    return;
  }
  for (const auto& network : networks4_) {
    originateNetwork(network);
  }
  for (const auto& network : networks6_) {
    originateNetwork(network);
  }
  /*
   * Mark as originated only after a full successful pass. originateNetwork()
   * throws if a network references an unconfigured policy; leaving the guard
   * unset on a thrown config error keeps origination all-or-nothing rather
   * than marking the switch done after a partial pass (addOriginatedRoute is
   * idempotent per-prefix).
   */
  routesOriginated_ = true;
}

void BgpSwitch::originateNetwork(const thrift::BgpNetwork& network) {
  const auto prefix = folly::IPAddress::createNetwork(*network.prefix());
  auto path = buildOriginatedPath(prefix, network);
  if (!path) {
    /* Invalid origin value; buildOriginatedPath already logged the error. */
    return;
  }

  std::string policyName;
  std::shared_ptr<const BgpPath> originatedPath = std::move(path);
  if (network.policy_name().has_value() && !network.policy_name()->empty()) {
    policyName = *network.policy_name();
    /*
     * Reuse the shared policy-application helper (also used by ingress/egress)
     * so the policy-lookup / applyPolicy / null-attrs-reject flow lives in one
     * place. applyRoutePolicy throws if the policy is not configured.
     */
    originatedPath = applyRoutePolicy(policyName, prefix, originatedPath);
    if (!originatedPath) {
      XLOGF(
          DBG2,
          "BgpSwitch {}: origination policy {} rejected prefix {}",
          name_,
          policyName,
          *network.prefix());
      return;
    }
  }

  std::const_pointer_cast<BgpPath>(originatedPath)->publish();
  routingTable_.addOriginatedRoute(prefix, originatedPath, policyName);
}

std::vector<folly::CIDRNetwork> BgpSwitch::advertisablePrefixes() const {
  folly::F14FastSet<folly::CIDRNetwork> seen;
  std::vector<folly::CIDRNetwork> prefixes;
  const auto add = [&](const folly::CIDRNetwork& prefix) {
    if (seen.insert(prefix).second) {
      prefixes.push_back(prefix);
    }
  };
  for (const auto& net : networks4_) {
    add(folly::IPAddress::createNetwork(*net.prefix()));
  }
  for (const auto& net : networks6_) {
    add(folly::IPAddress::createNetwork(*net.prefix()));
  }
  for (const auto& peer : peers_) {
    for (const auto& received : peer.receivedRoutes()) {
      add(received.cidr);
    }
  }
  return prefixes;
}

BgpPeer* FOLLY_NULLABLE
BgpSwitch::findPeerToward(const folly::IPAddress& neighborAddr) {
  for (auto& peer : peers_) {
    if (peer.peerIp() == neighborAddr) {
      return &peer;
    }
  }
  return nullptr;
}

std::shared_ptr<const BgpPath> BgpSwitch::applyRoutePolicy(
    const std::string& policyName,
    const folly::CIDRNetwork& prefix,
    const std::shared_ptr<const BgpPath>& path) {
  if (!policyManager_ || !policyManager_->isPolicyPresent(policyName)) {
    throw std::runtime_error(
        fmt::format(
            "BgpSwitch {}: policy '{}' for prefix {} is not configured",
            name_,
            policyName,
            folly::IPAddress::networkToString(prefix)));
  }
  /*
   * Published paths are immutable; applyPolicy clones on modification, so it
   * never mutates the input. The const_cast only satisfies the API's
   * shared_ptr<BgpPath> parameter.
   */
  auto inAttrs = std::const_pointer_cast<BgpPath>(path);
  auto policyOut = policyManager_->applyPolicy(
      policyName, PolicyInMessage({prefix}, inAttrs));
  const auto it = policyOut.result.find(prefix);
  if (it == policyOut.result.end() || !it->second->attrs) {
    return nullptr;
  }
  return it->second->attrs;
}

// TODO: track split-horizon + route-reflection for M2 parity
void BgpSwitch::propagateRoutes() {
  const auto prefixes = advertisablePrefixes();
  for (auto& peer : peers_) {
    if (!peer.isLinked()) {
      continue;
    }
    const PeerType egressType = peer.peerType();
    std::vector<RouteUpdate> updates;
    for (const auto& prefix : prefixes) {
      const SimRibEntry* entry = routingTable_.getEntry(prefix);
      if (entry == nullptr) {
        continue;
      }
      const auto& bestPath = entry->getBestPath();
      if (!bestPath) {
        continue;
      }
      std::shared_ptr<const BgpPath> path = bestPath->attrs;
      if (!peer.egressPolicyName().empty()) {
        path = applyRoutePolicy(peer.egressPolicyName(), prefix, path);
        if (!path) {
          continue; // egress policy rejected this prefix for this peer
        }
      }
      /*
       * Prepend our ASN (for (confed-)external peers) before advertising so the
       * AS-path grows across hops and downstream loop detection can fire.
       */
      path = applyEgressAsPath(path, egressType, localAsn_, localConfedAsn_);
      updates.push_back(RouteUpdate{prefix, std::move(path)});
    }
    if (!updates.empty()) {
      peer.getRemoteSwitch()->receiveRoutes(peer, name_, updates);
    }
  }
}

void BgpSwitch::receiveRoutes(
    const BgpPeer& fromPeer,
    const std::string& fromSwitchName,
    const std::vector<RouteUpdate>& routes) {
  BgpPeer* recvPeer = findPeerToward(fromPeer.localIp());
  if (recvPeer == nullptr) {
    XLOGF(
        DBG2,
        "BgpSwitch {}: received routes from {} but no local peer faces sender "
        "address {}; dropping",
        name_,
        fromSwitchName,
        fromPeer.localIp().str());
    return;
  }

  const uint64_t senderRouterId = fromPeer.routerId();
  const RouteOrigin origin = toRouteOrigin(recvPeer->peerType());
  const std::string peerKey = recvPeer->peerIp().str();
  const bool medMissingAsWorst = routingTable_.config().enableMedMissingAsWorst;

  for (const auto& update : routes) {
    std::shared_ptr<const BgpPath> path = update.path;
    if (path == nullptr) {
      continue;
    }

    /*
     * AS-path loop detection runs before ingress policy, mirroring production
     * AdjRib::validateAttributesIn (the loop check precedes import policy):
     * drop routes that already carry our ASN.
     */
    if (const auto loopAsn = asPathLoopAsn(
            *path, localAsn_, recvPeer->localAsn(), localConfedAsn_)) {
      XLOGF(
          DBG2,
          "BgpSwitch {}: dropping looped route {} from peer {} (remoteAsn={}) "
          "via {}: AS {} already in AS-path "
          "(localAsn={} peerLocalAs={} confedAsn={})",
          name_,
          folly::IPAddress::networkToString(update.prefix),
          recvPeer->peerIp().str(),
          recvPeer->remoteAsn(),
          fromSwitchName,
          *loopAsn,
          localAsn_,
          recvPeer->localAsn(),
          localConfedAsn_.has_value() ? std::to_string(*localConfedAsn_)
                                      : "none");
      continue;
    }

    if (!recvPeer->ingressPolicyName().empty()) {
      path =
          applyRoutePolicy(recvPeer->ingressPolicyName(), update.prefix, path);
      if (!path) {
        continue; // ingress policy rejected this prefix
      }
    }

    /*
     * Idempotent: skip re-processing a path identical to what this peer already
     * advertised. This keeps the convergence loop terminating and avoids
     * duplicate received-route bookkeeping. The comparison uses the post-policy
     * path so it matches the post-policy path stored below.
     */
    const SimRibEntry* existing = routingTable_.getEntry(update.prefix);
    if (existing != nullptr) {
      const auto& existingPaths = existing->getAllPaths();
      const auto existingIt = existingPaths.find(peerKey);
      if (existingIt != existingPaths.end() && existingIt->second->attrs &&
          *existingIt->second->attrs == *path) {
        continue;
      }
    }

    /*
     * Publish the path before storing it (mirrors originateNetwork): a later
     * egress-policy pass in propagateRoutes const-casts the stored path and
     * feeds it to PolicyManager, which clones-on-write only for published
     * inputs. Storing an unpublished policy output risks in-place mutation that
     * would corrupt both the RIB entry and the recorded ReceivedRoute.
     * publish() is idempotent, so re-publishing an already-published sender
     * path is a no-op.
     */
    auto publishedPath = std::const_pointer_cast<BgpPath>(path);
    publishedPath->publish();
    path = std::move(publishedPath);

    recvPeer->addReceivedRoute(
        ReceivedRoute{update.prefix, fromSwitchName, path});
    routingTable_.insertPath(
        update.prefix,
        peerKey,
        std::make_shared<SimRouteInfo>(
            update.prefix,
            path,
            peerKey,
            senderRouterId,
            recvPeer->peerIp(),
            origin,
            medMissingAsWorst));
  }
}

bool BgpSwitch::runBestPathSelection() {
  const auto prefixes = advertisablePrefixes();

  /*
   * Snapshot the current best-path identity per prefix before selection,
   * pairing each snapshot with its prefix so the before/after comparison needs
   * no positional indexing across parallel vectors (which could silently drift
   * and is what the array-bounds linter flags). A flat vector is used rather
   * than a CIDRNetwork-keyed map: the BGP memory rules prohibit ad-hoc
   * prefix-keyed collections that mirror RIB scale (each costs ~1.8GB at 30M
   * routes), and this snapshot is transient per selection batch.
   *
   * The snapshot holds an owning shared_ptr (not a raw pointer): selection can
   * deselect and destroy a previously-best SimRouteInfo, after which a raw
   * snapshot pointer would dangle. Even reading the dangling pointer value for
   * the != comparison is undefined behavior once the object's lifetime ends, so
   * we retain ownership to guarantee the snapshotted objects outlive the
   * comparison below.
   */
  std::vector<
      std::pair<folly::CIDRNetwork, std::shared_ptr<const SimRouteInfo>>>
      before;
  before.reserve(prefixes.size());
  for (const auto& prefix : prefixes) {
    const SimRibEntry* entry = routingTable_.getEntry(prefix);
    before.emplace_back(
        prefix,
        entry ? entry->getBestPath() : std::shared_ptr<const SimRouteInfo>{});
  }

  routingTable_.runBestPathSelection();

  bool changed = false;
  for (const auto& [prefix, previous] : before) {
    const SimRibEntry* entry = routingTable_.getEntry(prefix);
    const SimRouteInfo* now =
        (entry && entry->getBestPath()) ? entry->getBestPath().get() : nullptr;

    /*
     * Best path selection must account for all paths if/when UCOMP/LBW/ECMP
     * aggregates are supported.
     */
    if (now != previous.get()) {
      changed = true;
      break;
    }
  }
  bestPathChanged_ = changed;
  return changed;
}

} // namespace facebook::bgp
