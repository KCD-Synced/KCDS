#include "kcd2s/integration/demo.hpp"

#include "kcd2s/testkit.hpp"

using kcd2s::integration::DemoConfig;
using kcd2s::integration::run_demo;

KCD2S_TEST(demo, converges_lossless) {
  DemoConfig cfg;
  cfg.clients = 3;
  cfg.npcs = 20;
  cfg.ticks = 20;
  auto r = run_demo(cfg);
  KCD2S_CHECK(r.converged);
  KCD2S_CHECK_EQ(r.world_live, static_cast<std::size_t>(20 + 3));  // npcs + 3 avatars
  for (std::size_t s : r.mirror_sizes)
    KCD2S_CHECK_EQ(s, r.world_live);
  KCD2S_CHECK(r.snapshot_bytes > 0);
  KCD2S_CHECK_EQ(r.events_delivered, 3);  // one welcome per client
  KCD2S_CHECK_EQ(r.rpc_ok, 3);            // one ping response per client
}

KCD2S_TEST(demo, converges_parallel) {
  DemoConfig cfg;
  cfg.clients = 4;
  cfg.npcs = 30;
  cfg.ticks = 20;
  cfg.parallel = true;
  auto r = run_demo(cfg);
  KCD2S_CHECK(r.converged);
}

KCD2S_TEST(demo, converges_under_loss) {
  DemoConfig cfg;
  cfg.clients = 2;
  cfg.npcs = 10;
  cfg.ticks = 80;  // more ticks to recover dropped constructs
  cfg.loss = 0.25;
  auto r = run_demo(cfg);
  KCD2S_CHECK(r.converged);
}

KCD2S_TEST(demo, soak_1k_limited_interest) {
  kcd2s::integration::SoakConfig cfg;
  cfg.entities = 1000;
  cfg.clients = 4;
  cfg.move_ticks = 30;
  cfg.settle_ticks = 40;
  cfg.range = 150.0f;
  auto r = kcd2s::integration::run_soak(cfg);
  KCD2S_CHECK_EQ(r.world_live, static_cast<std::size_t>(1000 + 4));
  KCD2S_CHECK(r.correct);  // every mirrored entity matches the world
  KCD2S_CHECK(r.limited);  // interest actually limits scope (< all entities)
  KCD2S_CHECK(r.max_scope < r.world_live);
  KCD2S_CHECK(r.snapshot_bytes > 0);
  std::fprintf(
      stderr,
      "    [soak] live=%zu avg_scope=%.0f min=%zu max=%zu bytes=%llu ticks=%d tick_ms=%.1f\n",
      r.world_live, r.avg_scope, r.min_scope, r.max_scope,
      static_cast<unsigned long long>(r.snapshot_bytes), r.total_ticks, r.server_tick_ms);
}

KCD2S_TEST(demo, soak_byte_budget_caps_then_converges) {
  // Scope (range 250 over 1000^2 with 1000 entities) is far bigger than the budget, so the budget
  // throttles each tick; enough settle ticks then let the backlog fully drain.
  kcd2s::integration::SoakConfig cfg;
  cfg.entities = 1000;
  cfg.clients = 2;
  cfg.move_ticks = 20;
  cfg.settle_ticks = 160;
  cfg.range = 250.0f;
  cfg.byte_budget = 1500;  // soft per-client per-tick cap
  auto r = kcd2s::integration::run_soak(cfg);
  KCD2S_CHECK(r.correct);  // converges with no desync despite the cap
  KCD2S_CHECK(r.limited);
  KCD2S_CHECK(r.max_scope > 0);
  // Per-tick bytes stayed under the cap (+ at most one overflowing entry per client per tick).
  const std::uint64_t ceiling = static_cast<std::uint64_t>(cfg.byte_budget + 400) *
                                static_cast<std::uint64_t>(cfg.clients) *
                                static_cast<std::uint64_t>(r.total_ticks);
  KCD2S_CHECK(r.snapshot_bytes <= ceiling);
}
