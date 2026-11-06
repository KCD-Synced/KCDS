#include "kcd2s/integration/demo.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>
#include <string>
#include <variant>

#include "kcd2s/integration/client_bridge_driver.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/netbus.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/transport/connection.hpp"
#include "kcd2s/transport/datagram.hpp"
#include "kcd2s/wire/packet.hpp"

namespace kcd2s::integration {

namespace {

using namespace kcd2s::replication;
using kcd2s::transport::Connection;
using kcd2s::transport::LinkParams;
using kcd2s::transport::make_sim_pair;
using kcd2s::transport::SimLink;

SchemaRegistry demo_schema() {
  SchemaRegistry reg;
  reg.define("Human")
      .field("position", FieldKind::Vec3, Priority::High, Authority::Owner)
      .field("health", FieldKind::F32, Priority::Med, Authority::Server)
      .field("name", FieldKind::Str, Priority::Low, Authority::Server);
  return reg;
}

bool entity_matches(const World& w, const Mirror& m, core::EntityId e) {
  const auto* me = m.get(e);
  const auto* s = w.slot(e);
  if (!me || !s)
    return false;
  const auto* td = w.schema().get(s->type);
  for (const auto& fd : td->fields)
    if (!(me->fields[fd.id] == *w.read(e, fd.id)))
      return false;
  return true;
}

// A bridge sink that just counts (stands in for the engine).
struct CountingSink : ITransformSink {
  int binds = 0, unbinds = 0, applied = 0;
  void on_bind(core::EntityId) override {
    ++binds;
  }
  void on_unbind(core::EntityId) override {
    ++unbinds;
  }
  void apply(const std::vector<bridge::TransformApply>& t) override {
    applied += static_cast<int>(t.size());
  }
};

}  // namespace

DemoResult run_demo(const DemoConfig& cfg) {
  SchemaRegistry reg = demo_schema();
  Server server(reg);
  if (cfg.parallel)
    server.enable_parallel();
  World& w = server.world();
  const auto human = reg.get("Human")->id;

  w.set_tick(1);
  for (int i = 0; i < cfg.npcs; ++i) {
    const auto e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i % 40), 0, static_cast<float>(i / 40)});
    w.write(e, 1, static_cast<float>(i) / (cfg.npcs + 1));
    w.write(e, 2, std::string("npc-") + std::to_string(i));
  }

  struct Peer {
    std::unique_ptr<SimLink> rs, rc, ns, nc;
    std::unique_ptr<Connection> srv_bus_conn, cli_bus_conn;
    std::unique_ptr<ClientPeer> peer;
    std::unique_ptr<ClientBridgeDriver> driver;
    std::unique_ptr<NetBus> srv_bus, cli_bus;
    std::unique_ptr<CountingSink> sink;
    core::EntityId avatar = 0;
    PeerId pid = 0;
    int events = 0;
    int rpc = 0;
  };
  std::vector<std::unique_ptr<Peer>> peers;

  LinkParams a2b;
  a2b.loss = cfg.loss;
  a2b.seed = 0x5151;

  for (int i = 0; i < cfg.clients; ++i) {
    auto p = std::make_unique<Peer>();
    p->avatar = w.spawn(human);
    w.write(p->avatar, 0, Vec3{static_cast<float>(i * 3), 0, 0});

    auto rpair = make_sim_pair(a2b, LinkParams{});
    p->rs = std::move(rpair.first);
    p->rc = std::move(rpair.second);
    p->pid = server.add_client(p->rs.get(), p->avatar, 1.0e6f);
    server.set_owner(p->avatar, p->pid);
    p->peer = std::make_unique<ClientPeer>(reg, p->rc.get());

    p->sink = std::make_unique<CountingSink>();
    p->driver = std::make_unique<ClientBridgeDriver>(reg, *p->sink);

    // A separate lossless pair for the resource NetBus (channel 2).
    auto npair = make_sim_pair({}, {});
    p->ns = std::move(npair.first);
    p->nc = std::move(npair.second);
    p->srv_bus_conn = std::make_unique<Connection>(p->ns.get());
    p->cli_bus_conn = std::make_unique<Connection>(p->nc.get());
    p->srv_bus = std::make_unique<NetBus>(*p->srv_bus_conn);
    p->cli_bus = std::make_unique<NetBus>(*p->cli_bus_conn);
    Peer* raw = p.get();
    p->cli_bus->on("welcome", [raw](const sdk::Value::Array&) { ++raw->events; });
    p->srv_bus->register_rpc(
        "ping", [](const sdk::Value::Array&) { return sdk::Value(sdk::EntityRef{1}); });
    peers.push_back(std::move(p));
  }

  const std::uint8_t ev = static_cast<std::uint8_t>(wire::Channel::Events);

  const double dt = 1.0 / 60.0;  // a real wall clock so RTT-timed reliable retransmit can fire
  for (int t = 2; t <= cfg.ticks + 1; ++t) {
    const double now = t * dt;
    w.set_tick(static_cast<core::Tick>(t));
    // Owners drive their avatars; server moves one NPC to create ongoing deltas.
    for (auto& p : peers)
      p->peer->send_input(p->avatar,
                          {{core::FieldId{0}, FieldValue{Vec3{static_cast<float>(t), 1, 2}}}});
    if (!peers.empty()) {
      // NPC 0 wiggles.
      // (owned by server; AUTH_SERVER-free position is fine to move server-side)
    }

    server.tick(now);
    for (auto& p : peers) {
      p->peer->tick(now);
      p->driver->sync(p->peer->mirror());
    }

    // Resource NetBus: server greets at t==3, client pings at t==4.
    for (auto& p : peers) {
      if (t == 3)
        p->srv_bus->emit("welcome", {sdk::Value("hi")});
      if (t == 4)
        p->cli_bus->call("ping", {}, [raw = p.get()](const sdk::Value&) { ++raw->rpc; });
      p->srv_bus_conn->flush(now);
      p->cli_bus_conn->flush(now);
      for (auto& m : p->cli_bus_conn->receive(now))
        if (m.channel == ev)
          p->cli_bus->feed(m.bytes);
      for (auto& m : p->srv_bus_conn->receive(now))
        if (m.channel == ev)
          p->srv_bus->feed(m.bytes);
    }
  }

  DemoResult r;
  r.snapshot_bytes = server.snapshot_bytes_sent();
  r.world_live = w.entity_count();
  r.converged = true;
  for (auto& p : peers) {
    r.mirror_sizes.push_back(p->peer->mirror().size());
    for (auto e : w.live_entities())
      if (!entity_matches(w, p->peer->mirror(), e))
        r.converged = false;
    r.events_delivered += p->events;
    r.rpc_ok += p->rpc;
  }
  return r;
}

SoakResult run_soak(const SoakConfig& cfg) {
  SchemaRegistry reg = demo_schema();
  Server server(reg, InterestConfig{64.0f});
  if (cfg.parallel)
    server.enable_parallel();
  if (cfg.byte_budget)
    server.set_byte_budget(cfg.byte_budget);
  World& w = server.world();
  const auto human = reg.get("Human")->id;

  std::mt19937 rng(0x50AC1234u);
  std::uniform_real_distribution<float> pos(0.0f, cfg.world_extent);

  w.set_tick(1);
  std::vector<core::EntityId> ents;
  ents.reserve(cfg.entities);
  for (int i = 0; i < cfg.entities; ++i) {
    const auto e = w.spawn(human);
    w.write(e, 0, Vec3{pos(rng), 0, pos(rng)});
    w.write(e, 1, 1.0f);
    w.write(e, 2, std::string("e") + std::to_string(i));
    ents.push_back(e);
  }

  struct SoakPeer {
    std::unique_ptr<SimLink> rs, rc;
    std::unique_ptr<ClientPeer> peer;
    core::EntityId viewer = 0;
  };
  std::vector<std::unique_ptr<SoakPeer>> peers;
  for (int i = 0; i < cfg.clients; ++i) {
    auto p = std::make_unique<SoakPeer>();
    p->viewer = w.spawn(human);
    // Spread viewers across the world so each sees a different, limited subset.
    const float fx = (static_cast<float>(i) + 0.5f) / cfg.clients * cfg.world_extent;
    w.write(p->viewer, 0, Vec3{fx, 0, cfg.world_extent * 0.5f});
    auto pr = make_sim_pair({}, {});
    p->rs = std::move(pr.first);
    p->rc = std::move(pr.second);
    server.add_client(p->rs.get(), p->viewer, cfg.range);
    p->peer = std::make_unique<ClientPeer>(reg, p->rc.get());
    peers.push_back(std::move(p));
  }

  SoakResult r;
  core::Tick frame = 1;
  using clock = std::chrono::steady_clock;
  double tick_ns = 0.0;

  const double dt = 1.0 / 60.0;
  auto run_ticks = [&](int n, bool move) {
    std::uniform_int_distribution<int> pick(0, cfg.entities - 1);
    for (int t = 0; t < n; ++t) {
      w.set_tick(++frame);
      const double now = frame * dt;
      if (move) {
        for (int k = 0; k < cfg.entities / 10; ++k) {  // move ~10% of entities each tick
          const int idx = pick(rng);
          w.write(ents[idx], 0, Vec3{pos(rng), 0, pos(rng)});
        }
      }
      const auto t0 = clock::now();
      server.tick(now);
      tick_ns += std::chrono::duration<double, std::nano>(clock::now() - t0).count();
      for (auto& p : peers)
        p->peer->tick(now);
    }
  };

  run_ticks(cfg.move_ticks, /*move=*/true);
  run_ticks(cfg.settle_ticks, /*move=*/false);  // no movement -> mirrors settle

  r.world_live = w.entity_count();
  r.snapshot_bytes = server.snapshot_bytes_sent();
  r.server_tick_ms = tick_ns / 1e6;
  r.total_ticks = cfg.move_ticks + cfg.settle_ticks;
  r.correct = true;
  std::size_t total_scope = 0;
  r.min_scope = r.world_live;
  r.max_scope = 0;
  for (auto& p : peers) {
    const std::size_t sz = p->peer->mirror().size();
    total_scope += sz;
    r.min_scope = std::min(r.min_scope, sz);
    r.max_scope = std::max(r.max_scope, sz);
    if (sz < r.world_live)
      r.limited = true;
    // Every entity the client holds must match the authoritative world.
    for (const auto& [id, ent] : p->peer->mirror().entities()) {
      (void)ent;
      if (!entity_matches(w, p->peer->mirror(), id))
        r.correct = false;
    }
  }
  r.avg_scope = peers.empty() ? 0.0 : static_cast<double>(total_scope) / peers.size();
  return r;
}

}  // namespace kcd2s::integration
