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

/*
 * Tests for BgpSimulator — loading switches from configs and resolving peer
 * links from config addresses into direct in-process remote-switch references.
 */

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <folly/IPAddress.h>

#include <folly/logging/LoggerDB.h>
#include <folly/logging/test/TestLogHandler.h>

#include "configerator/structs/neteng/fboss/bgp/gen-cpp2/bgp_config_types.h"
#include "configerator/structs/neteng/fboss/bgp/if/gen-cpp2/bgp_attr_types.h"
#include "neteng/fboss/bgp/cpp/common/BgpPath.h"
#include "neteng/fboss/bgp/cpp/config/Config.h"
#include "neteng/fboss/bgp/cpp/config/ConfigUtils.h"
#include "neteng/fboss/bgp/cpp/lib/BgpStructs.h"
#include "neteng/fboss/bgp/cpp/sim/BgpSimulator.h"
#include "neteng/fboss/bgp/cpp/sim/BgpSwitch.h"
#include "neteng/fboss/bgp/cpp/tests/Utils.h"

namespace facebook::bgp {

namespace {

thrift::BgpConfig makeConfig(const std::string& routerId, int64_t localAs) {
  thrift::BgpConfig config;
  config.router_id() = routerId;
  config.local_as_4_byte() = localAs;
  return config;
}

// A peer talking to neighbor `peerAddr` over local address `localAddr`.
thrift::BgpPeer makePeer(
    const std::string& peerAddr,
    const std::string& localAddr) {
  thrift::BgpPeer peer;
  peer.peer_addr() = peerAddr;
  peer.local_addr() = localAddr;
  peer.next_hop4() = localAddr;
  peer.next_hop6() = "::1";
  peer.remote_as_4_byte() = 65000;
  return peer;
}

thrift::BgpNetwork makeNetwork(const std::string& prefix) {
  thrift::BgpNetwork net;
  net.prefix() = prefix;
  return net;
}

// Borrow the sample config's policies (includes PROPAGATE_NOTHING).
auto loadSamplePolicies() {
  const auto path = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");
  Config config(
      path, /*peerSubnetLbwMap=*/std::nullopt, /*populateConfigDb=*/false);
  return *config.getConfig().policies();
}

// Build a published BgpPath whose AS-path is the given AS sequence.
std::shared_ptr<const BgpPath> makePathWithAsPath(
    const std::vector<uint32_t>& asns) {
  neteng::fboss::bgp_attr::TAsPathSeg seg;
  seg.seg_type() = neteng::fboss::bgp_attr::TAsPathSegType::AS_SEQUENCE;
  seg.asns_4_byte() = std::vector<int64_t>(asns.begin(), asns.end());
  nettools::bgplib::BgpPathC pathC;
  pathC.nexthop = folly::IPAddress("10.0.0.1");
  nettools::bgplib::BgpAttributesC attrs;
  attrs.origin = nettools::bgplib::BgpAttrOrigin::BGP_ORIGIN_IGP;
  attrs.localPref = 100;
  attrs.asPath = createBgpAttrAsPathDedup({seg});
  pathC.attrs = std::move(attrs);
  auto path = std::make_shared<BgpPath>(static_cast<BgpPathFields>(pathC));
  path->publish();
  return path;
}

} // namespace

class BgpSimulatorTest : public ::testing::Test {};

/*
 * Two switches whose peers point at each other's local addresses resolve to
 * reciprocal remote-switch links.
 */
TEST_F(BgpSimulatorTest, TwoSwitchReciprocalLinks) {
  thrift::BgpConfig cfgA = makeConfig("10.0.0.1", 65000);
  cfgA.peers() = {makePeer(/*peerAddr=*/"10.0.0.2", /*localAddr=*/"10.0.0.1")};
  thrift::BgpConfig cfgB = makeConfig("10.0.0.2", 65000);
  cfgB.peers() = {makePeer(/*peerAddr=*/"10.0.0.1", /*localAddr=*/"10.0.0.2")};

  BgpSimulator sim;
  auto switchA = std::make_shared<BgpSwitch>("switchA", cfgA);
  auto switchB = std::make_shared<BgpSwitch>("switchB", cfgB);
  sim.addSwitch(switchA);
  sim.addSwitch(switchB);

  sim.resolvePeerLinks();

  ASSERT_EQ(1u, switchA->peers().size());
  ASSERT_EQ(1u, switchB->peers().size());
  EXPECT_TRUE(switchA->peers().front().isLinked());
  EXPECT_EQ(switchB.get(), switchA->peers().front().getRemoteSwitch().get());
  EXPECT_TRUE(switchB->peers().front().isLinked());
  EXPECT_EQ(switchA.get(), switchB->peers().front().getRemoteSwitch().get());
}

/*
 * A peer whose neighbor address belongs to no modeled switch is left unlinked
 * (models an external / unmodeled neighbor) rather than failing.
 */
TEST_F(BgpSimulatorTest, UnresolvedPeerLeftUnlinked) {
  thrift::BgpConfig cfg = makeConfig("10.0.0.1", 65000);
  cfg.peers() = {
      makePeer(/*peerAddr=*/"10.99.99.99", /*localAddr=*/"10.0.0.1")};

  BgpSimulator sim;
  sim.addSwitch(std::make_shared<BgpSwitch>("switchA", cfg));

  sim.resolvePeerLinks();

  EXPECT_FALSE(sim.switches().front()->peers().front().isLinked());
}

/*
 * loadConfigs() parses a real config file into a BgpSwitch named after the
 * file stem.
 */
TEST_F(BgpSimulatorTest, LoadConfigsFromFile) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");

  BgpSimulator sim;
  sim.loadConfigs({configPath});

  ASSERT_EQ(1u, sim.numSwitches());
  EXPECT_EQ("stand_alone_conf", sim.switches().front()->name());
  EXPECT_EQ(65401u, sim.switches().front()->localAsn());
}

/*
 * Two config paths that share a file stem (here the same sample config loaded
 * twice) yield two switches with the same name; both are still loaded and
 * loadConfigs() emits a WARN about the collision. The warning's exact wording
 * is an implementation detail, so assert only that a WARN-level message was
 * emitted rather than coupling to its text.
 */
TEST_F(BgpSimulatorTest, DuplicateSwitchNamesWarn) {
  const auto configPath = getAbsoluteFilePath(
      "neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json");

  auto logHandler = std::make_shared<folly::TestLogHandler>();
  folly::LoggerDB::get().getCategory("")->addHandler(logHandler);
  folly::LoggerDB::get().setLevel("", folly::LogLevel::WARN);

  BgpSimulator sim;
  sim.loadConfigs({configPath, configPath});

  EXPECT_EQ(2u, sim.numSwitches());
  const auto& messages = logHandler->getMessages();
  EXPECT_TRUE(
      std::any_of(
          messages.begin(),
          messages.end(),
          [](const auto& entry) {
            return entry.first.getLevel() >= folly::LogLevel::WARN;
          }))
      << "expected a WARN about the duplicate switch name";
}

/*
 * A two-switch iBGP topology where switch A originates 10.50.0.0/24 and
 * advertises it to switch B, parameterized over the policy attachment so the
 * on/off matrix is explicit:
 *   - NoPolicy:     the route reaches B's RIB and receiving peer.
 *   - EgressBlocks: A's egress PROPAGATE_NOTHING stops it leaving A.
 *   - IngressBlocks: B's ingress PROPAGATE_NOTHING drops it on reception.
 */
struct PropagationPolicyCase {
  std::string name;
  bool egressBlocksOnA;
  bool ingressBlocksOnB;
  bool expectReceivedOnB;
};

class BgpPropagationPolicyTest
    : public ::testing::TestWithParam<PropagationPolicyCase> {};

TEST_P(BgpPropagationPolicyTest, PolicyControlsPropagation) {
  const auto& tc = GetParam();

  thrift::BgpConfig cfgA = makeConfig("10.0.0.1", 65000);
  thrift::BgpPeer peerA = makePeer("10.0.0.2", "10.0.0.1");
  if (tc.egressBlocksOnA) {
    cfgA.policies() = loadSamplePolicies();
    peerA.egress_policy_name() = "PROPAGATE_NOTHING";
  }
  cfgA.peers() = {peerA};
  cfgA.networks4() = {makeNetwork("10.50.0.0/24")};

  thrift::BgpConfig cfgB = makeConfig("10.0.0.2", 65000);
  thrift::BgpPeer peerB = makePeer("10.0.0.1", "10.0.0.2");
  if (tc.ingressBlocksOnB) {
    cfgB.policies() = loadSamplePolicies();
    peerB.ingress_policy_name() = "PROPAGATE_NOTHING";
  }
  cfgB.peers() = {peerB};

  BgpSimulator sim;
  sim.addSwitch(std::make_shared<BgpSwitch>("A", cfgA));
  sim.addSwitch(std::make_shared<BgpSwitch>("B", cfgB));
  sim.resolvePeerLinks();
  BgpSwitch& a = *sim.switches()[0];
  BgpSwitch& b = *sim.switches()[1];

  a.originateRoutes();
  a.routingTable().runBestPathSelection();
  a.propagateRoutes();

  const auto prefix = folly::IPAddress::createNetwork("10.50.0.0/24");
  if (tc.expectReceivedOnB) {
    EXPECT_NE(nullptr, b.routingTable().getEntry(prefix));
    ASSERT_EQ(1u, b.peers().front().receivedRoutes().size());
    const ReceivedRoute& received = b.peers().front().receivedRoutes().front();
    EXPECT_EQ(prefix, received.cidr);
    EXPECT_EQ("A", received.fromSwitch);
  } else {
    EXPECT_EQ(nullptr, b.routingTable().getEntry(prefix));
    EXPECT_TRUE(b.peers().front().receivedRoutes().empty());
  }
}

INSTANTIATE_TEST_SUITE_P(
    Policies,
    BgpPropagationPolicyTest,
    ::testing::Values(
        PropagationPolicyCase{/*name=*/"NoPolicy",
                              /*egressBlocksOnA=*/false,
                              /*ingressBlocksOnB=*/false,
                              /*expectReceivedOnB=*/true},
        PropagationPolicyCase{/*name=*/"EgressBlocks",
                              /*egressBlocksOnA=*/true,
                              /*ingressBlocksOnB=*/false,
                              /*expectReceivedOnB=*/false},
        PropagationPolicyCase{/*name=*/"IngressBlocks",
                              /*egressBlocksOnA=*/false,
                              /*ingressBlocksOnB=*/true,
                              /*expectReceivedOnB=*/false}),
    [](const ::testing::TestParamInfo<PropagationPolicyCase>& info) {
      return info.param.name;
    });

/*
 * On eBGP advertisement a switch prepends its own ASN to the AS-path (mirrors
 * production AdjRibCommon::updateAsPathAttributesCommon). A (AS 65000) eBGP to
 * B (AS 65001): the route A originates with an empty AS-path arrives at B
 * carrying A's ASN.
 */
TEST_F(BgpSimulatorTest, EbgpPropagationPrependsLocalAsn) {
  thrift::BgpConfig cfgA = makeConfig("10.0.0.1", 65000);
  thrift::BgpPeer peerAtoB = makePeer("10.0.0.2", "10.0.0.1");
  peerAtoB.remote_as_4_byte() = 65001; // eBGP toward B
  cfgA.peers() = {peerAtoB};
  cfgA.networks4() = {makeNetwork("10.50.0.0/24")};

  thrift::BgpConfig cfgB = makeConfig("10.0.0.2", 65001);
  thrift::BgpPeer peerBtoA = makePeer("10.0.0.1", "10.0.0.2");
  peerBtoA.remote_as_4_byte() = 65000; // eBGP toward A
  cfgB.peers() = {peerBtoA};

  BgpSimulator sim;
  sim.addSwitch(std::make_shared<BgpSwitch>("A", cfgA));
  sim.addSwitch(std::make_shared<BgpSwitch>("B", cfgB));
  sim.resolvePeerLinks();
  BgpSwitch& a = *sim.switches()[0];
  BgpSwitch& b = *sim.switches()[1];

  a.originateRoutes();
  a.routingTable().runBestPathSelection();
  a.propagateRoutes();

  ASSERT_EQ(1u, b.peers().front().receivedRoutes().size());
  const ReceivedRoute& received = b.peers().front().receivedRoutes().front();
  const auto& asPath = received.path->getAsPath().get();
  const std::vector<uint32_t> expectedAsSequence = {65000};
  ASSERT_EQ(1u, asPath.size());
  EXPECT_EQ(expectedAsSequence, asPath.front().asSequence);
}

/*
 * AS-path prepend makes loop detection reachable in a real propagation chain.
 * A (65000) and B (65001) are eBGP peers. A originates a route; B learns it
 * (AS-path [65000]). When B re-advertises back to A, B prepends 65001 giving
 * [65001, 65000]; A drops it because its own ASN is already present.
 */
TEST_F(BgpSimulatorTest, EbgpReadvertisementLoopIsDropped) {
  thrift::BgpConfig cfgA = makeConfig("10.0.0.1", 65000);
  thrift::BgpPeer peerAtoB = makePeer("10.0.0.2", "10.0.0.1");
  peerAtoB.remote_as_4_byte() = 65001;
  cfgA.peers() = {peerAtoB};
  cfgA.networks4() = {makeNetwork("10.50.0.0/24")};

  thrift::BgpConfig cfgB = makeConfig("10.0.0.2", 65001);
  thrift::BgpPeer peerBtoA = makePeer("10.0.0.1", "10.0.0.2");
  peerBtoA.remote_as_4_byte() = 65000;
  cfgB.peers() = {peerBtoA};

  BgpSimulator sim;
  sim.addSwitch(std::make_shared<BgpSwitch>("A", cfgA));
  sim.addSwitch(std::make_shared<BgpSwitch>("B", cfgB));
  sim.resolvePeerLinks();
  BgpSwitch& a = *sim.switches()[0];
  BgpSwitch& b = *sim.switches()[1];

  a.originateRoutes();
  a.routingTable().runBestPathSelection();
  a.propagateRoutes(); // A -> B

  b.routingTable().runBestPathSelection();
  b.propagateRoutes(); // B -> A re-advertises A's route back

  // A must not accept its own route back (AS-path loop on 65000).
  EXPECT_TRUE(a.peers().front().receivedRoutes().empty());
}

/*
 * A received route whose AS-path already contains the receiver's ASN is a loop
 * and is dropped.
 */
TEST_F(BgpSimulatorTest, ReceiveDropsAsPathLoop) {
  thrift::BgpConfig cfgB = makeConfig("10.0.0.2", 65001);
  cfgB.peers() = {makePeer("10.0.0.1", "10.0.0.2")};
  BgpSwitch b("B", cfgB);

  // The sender's peer (A's), whose local address identifies the session to B.
  thrift::BgpPeer aPeerCfg = makePeer("10.0.0.2", "10.0.0.1");
  BgpPeer fromPeerA(aPeerCfg);
  fromPeerA.setLocalAsn(65000);
  fromPeerA.setRouterId(folly::IPAddress("10.0.0.1").asV4().toLongHBO());

  const auto prefix = folly::IPAddress::createNetwork("10.50.0.0/24");
  // AS-path contains B's own ASN (65001) -> loop.
  std::vector<RouteUpdate> routes = {{prefix, makePathWithAsPath({65001})}};
  b.receiveRoutes(fromPeerA, "A", routes);

  EXPECT_EQ(nullptr, b.routingTable().getEntry(prefix));
  EXPECT_TRUE(b.peers().front().receivedRoutes().empty());
}

/*
 * Drive a 3-switch linear topology A--B--C (A originates 10.50.0.0/24) to a
 * bounded run() and assert the convergence outcome, parameterized over the
 * iteration cap:
 *   - ConvergesWellUnderCap: a generous cap lets the route reach every switch's
 *     RIB; C learns it from B and run() stops well before the cap.
 *   - DoesNotConvergeWithinCap: a cap of 1 cannot deliver A's prefix all the
 * way to C, so run() returns the cap and C is left without the route (covers
 * the non-convergence branch of run()).
 */
struct LinearConvergenceCase {
  std::string name;
  size_t maxIterations;
  bool expectConverged;
};

class BgpLinearConvergenceTest
    : public ::testing::TestWithParam<LinearConvergenceCase> {
 protected:
  /*
   * Build A--B--C linked as A(10.0.1.1)--(10.0.1.2)B(10.0.2.2)--(10.0.2.3)C,
   * with A originating 10.50.0.0/24, into sim_ and resolve the peer links.
   *
   * Note: this is an all-iBGP topology (local_as == remote_as). The simulator
   * intentionally does not model iBGP split-horizon / re-advertisement
   * restrictions (RFC 4271), so an iBGP-learned route is re-advertised to the
   * next iBGP peer. Termination here relies on receiveRoutes() idempotency and
   * AS-path loop detection rather than iBGP re-advertisement rules.
   */
  void buildLinearTopology() {
    thrift::BgpConfig cfgA = makeConfig("10.0.0.1", 65000);
    cfgA.peers() = {
        makePeer(/*peerAddr=*/"10.0.1.2", /*localAddr=*/"10.0.1.1")};
    cfgA.networks4() = {makeNetwork("10.50.0.0/24")};

    thrift::BgpConfig cfgB = makeConfig("10.0.0.2", 65000);
    cfgB.peers() = {
        makePeer(/*peerAddr=*/"10.0.1.1", /*localAddr=*/"10.0.1.2"),
        makePeer(/*peerAddr=*/"10.0.2.3", /*localAddr=*/"10.0.2.2")};

    thrift::BgpConfig cfgC = makeConfig("10.0.0.3", 65000);
    cfgC.peers() = {
        makePeer(/*peerAddr=*/"10.0.2.2", /*localAddr=*/"10.0.2.3")};

    sim_.addSwitch(std::make_shared<BgpSwitch>("A", cfgA));
    sim_.addSwitch(std::make_shared<BgpSwitch>("B", cfgB));
    sim_.addSwitch(std::make_shared<BgpSwitch>("C", cfgC));
    sim_.resolvePeerLinks();
  }

  BgpSimulator sim_;
};

TEST_P(BgpLinearConvergenceTest, RunRespectsIterationCap) {
  const auto& tc = GetParam();
  buildLinearTopology();

  const size_t iterations = sim_.run(tc.maxIterations);

  const auto prefix = folly::IPAddress::createNetwork("10.50.0.0/24");
  const BgpSwitch& c = *sim_.switches()[2];

  if (tc.expectConverged) {
    EXPECT_LT(iterations, tc.maxIterations); // converged before the safety cap

    // The originated prefix reached every switch's RIB with a best path.
    for (const auto& sw : sim_.switches()) {
      const SimRibEntry* entry = sw->routingTable().getEntry(prefix);
      ASSERT_NE(nullptr, entry) << sw->name();
      EXPECT_NE(nullptr, entry->getBestPath()) << sw->name();
    }
    // C's best path was learned from B (B's address on the B--C link).
    const SimRibEntry* cEntry = c.routingTable().getEntry(prefix);
    ASSERT_NE(nullptr, cEntry);
    const auto& cBestPath = cEntry->getBestPath();
    ASSERT_NE(nullptr, cBestPath);
    EXPECT_EQ("10.0.2.2", cBestPath->peerAddr);
  } else {
    EXPECT_EQ(tc.maxIterations, iterations); // hit the safety cap

    const SimRibEntry* cEntry = c.routingTable().getEntry(prefix);
    if (cEntry != nullptr) {
      // C may have an entry, but it must not have selected a best path.
      EXPECT_EQ(nullptr, cEntry->getBestPath());
    }
  }
}

INSTANTIATE_TEST_SUITE_P(
    LinearTopology,
    BgpLinearConvergenceTest,
    ::testing::Values(
        LinearConvergenceCase{/*name=*/"ConvergesWellUnderCap",
                              /*maxIterations=*/50,
                              /*expectConverged=*/true},
        LinearConvergenceCase{/*name=*/"DoesNotConvergeWithinCap",
                              /*maxIterations=*/1,
                              /*expectConverged=*/false}),
    [](const ::testing::TestParamInfo<LinearConvergenceCase>& info) {
      return info.param.name;
    });

} // namespace facebook::bgp
