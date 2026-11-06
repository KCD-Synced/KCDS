// Headless end-to-end demo (definition-of-done, headless half): a Server + N
// ClientPeers over SimLinks, each with an owned avatar driven by upstream input, a
// ClientBridgeDriver turning the mirror into transform applies, and a NetBus "resource" exchanging
// an event + an RPC. run_demo() is deterministic so both the CLI (demo_main) and the smoke test
// share it.
#ifndef KCD2S_INTEGRATION_DEMO_HPP
#define KCD2S_INTEGRATION_DEMO_HPP

#include <cstdint>
#include <vector>

namespace kcd2s::integration {

struct DemoConfig {
  int clients = 3;
  int npcs = 20;
  int ticks = 40;
  double loss = 0.0;  // server->client snapshot loss probability
  bool parallel = false;
};

struct DemoResult {
  bool converged = false;            // every client's mirror matches the world (within its AOI)
  std::uint64_t snapshot_bytes = 0;  // total snapshot bytes the server sent
  int events_delivered = 0;          // NetBus "welcome" events received by clients
  int rpc_ok = 0;                    // NetBus "ping" RPC responses received
  std::vector<std::size_t> mirror_sizes;
  std::size_t world_live = 0;
};

DemoResult run_demo(const DemoConfig& cfg);

// Soak / bandwidth benchmark: many entities, clients with LIMITED interest ranges, a movement phase
// then a settle phase. Measures bandwidth + build time and checks correctness/limiting.
struct SoakConfig {
  int entities = 1000;
  int clients = 4;
  int move_ticks = 40;
  int settle_ticks = 40;
  float world_extent = 1000.0f;  // entities spread over [0, extent)^2 on the x/z plane
  float range = 150.0f;          // client interest radius (limited, not see-all)
  std::uint32_t byte_budget = 0;
  bool parallel = true;
};

struct SoakResult {
  bool correct = false;  // every entity in every client's mirror matches the world
  bool limited = false;  // at least one client sees fewer than all entities (AOI works)
  std::uint64_t snapshot_bytes = 0;
  double server_tick_ms = 0.0;  // total wall time spent in Server::tick across the run
  std::size_t world_live = 0;
  std::size_t min_scope = 0;
  std::size_t max_scope = 0;
  double avg_scope = 0.0;
  int total_ticks = 0;
};

SoakResult run_soak(const SoakConfig& cfg);

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_DEMO_HPP
