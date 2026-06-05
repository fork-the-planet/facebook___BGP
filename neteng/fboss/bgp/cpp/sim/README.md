# BGP++ Simulator (`sim/`)

[comment]: # (The description below is subject to change)

## Overview

This directory contains the **BGP++ Routing Policy Simulator (BGP++ Sim)**, a C++
routing policy simulator that achieves 100% routing-table parity with Meta's network
emulation. It provides a fast, lightweight alternative to the full network emulation
used by the Routing Policy Verification (RPV / Maat) system, directly advancing safe
and rapid network change deployment.

Today, routing policy verification relies on full network emulation, which is slow and
resource-intensive — verifying a single BGP policy change across a production-scale DC
topology (300+ switches) requires spinning up an entire emulation environment. This
simulator replaces that with a purpose-built, policy-focused simulation that runs
end-to-end in under 5 minutes for a full DC topology.

## Architecture

The simulator follows a layered design with maximal reuse of production BGPCPP code:

- **Config Loading** — Thrift JSON deserialization via `SimpleJSONSerializer`, reusing
  BGPCPP's `Config` class to parse per-switch `bgpd.conf` files identically to production.
- **Topology Resolution** — Builds an address-to-switch map and resolves peer links
  without simulating TCP/transport.
- **Simulation Engine** — `BgpSwitch` (per-node state), `BgpPeer` (per-session config),
  and `RoutingTable` (local RIB with best-path selection). Uses synchronous iterative
  convergence rather than BGPCPP's fiber-based event-driven model.
- **Policy Evaluation** — Directly invokes BGPCPP's `PolicyManager::applyPolicy()` for
  ingress, egress, and origination policies. This is the key design choice: by reusing
  the production PolicyManager byte-for-byte, policy parity bugs are eliminated by
  construction.
- **Output** — Generates RIB dumps in the `EmulationRoutingDump` Thrift format,
  matching emulation output exactly for automated parity comparison.

## Key BGPCPP Dependencies

| Component | BUCK Target | Reuse Strategy |
|-----------|-------------|----------------|
| PolicyManager | `//neteng/fboss/bgp/cpp/policy:policy_manager` | Direct reuse |
| BgpPath | `//neteng/fboss/bgp/cpp/common:bgp_path` | Direct reuse |
| Config parsing | `//neteng/fboss/bgp/cpp/config:config` | Direct reuse |
| Best-path selection | (this directory) | Reimplemented (mirrors `RibEntry`) |

Components **not** reused: peer state machine (FSM), FIB programming, AdjRib lifecycle,
fiber/coro threading — these are unnecessary for offline policy simulation.

## Building

```bash
buck2 build //neteng/fboss/bgp/cpp/sim:bgp_config_summary
buck2 build //neteng/fboss/bgp/cpp/sim:bgp_simulator
```

## Running

`bgp_simulator` takes one or more per-switch config files, or a directory of
them, runs the simulation to convergence, and dumps each switch's RIB:

```bash
buck2 run //neteng/fboss/bgp/cpp/sim:bgp_simulator -- <config_dir | config_file>...
```

## Testing

```bash
buck2 test //neteng/fboss/bgp/cpp/sim:BgpSwitchTest
buck2 test //neteng/fboss/bgp/cpp/sim:BgpSimulatorTest
buck2 test //neteng/fboss/bgp/cpp/sim:BgpSimulatorCliTest
```

## References

- [BGP++ Simulator Design Document](https://docs.google.com/document/d/1cw_NQwBtkdVuwe3FGv5z6J6DQa_2rAJsR0qV21kiYm0)
- [BGPCPP codebase](https://www.internalfb.com/code/fbsource/fbcode/neteng/fboss/bgp/cpp/)
- [BGP++ Wiki](https://www.internalfb.com/wiki/Net_Systems/Teams/Routing_Protocol/BGP%2B%2B)
- [Maat / RPV Wiki](https://www.internalfb.com/wiki/Net_Systems/Teams/DNE/XFN_partnered_projects/Maat)
- Python reference simulator: `fbcode/neteng/routing_policy_simulator/`
