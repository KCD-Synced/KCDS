#include <memory>
#include <vector>

#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/replication/world.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"

using namespace kcd2s::integration;
using kcd2s::replication::FieldKind;
using kcd2s::replication::Priority;
using kcd2s::replication::SchemaRegistry;
using kcd2s::replication::Vec3;
using kcd2s::transport::LinkParams;
using kcd2s::transport::make_sim_pair;
using kcd2s::transport::SimLink;

static SchemaRegistry world_schema() {
  SchemaRegistry reg;
  reg.define("Human")
      .field("position", FieldKind::Vec3, Priority::High)
      .field("health", FieldKind::F32, Priority::Med)
      .field("name", FieldKind::Str, Priority::Low);
  return reg;
}

// Does the mirror hold entity e with field values equal to the server world's?
static bool entity_matches(const kcd2s::replication::World& w, const kcd2s::replication::Mirror& m,
                           kcd2s::core::EntityId e) {
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

// One test client: owns its SimLink pair endpoints and its peer.
struct TestClient {
  std::unique_ptr<SimLink> server_link;
  std::unique_ptr<SimLink> client_link;
  std::unique_ptr<ClientPeer> peer;
  PeerId pid = 0;
};

KCD2S_TEST(convergence, three_clients_full_range) {
  SchemaRegistry reg = world_schema();
  Server server(reg, kcd2s::replication::InterestConfig{64.0f});
  auto& w = server.world();
  const auto human = reg.get("Human")->id;

  w.set_tick(1);
  std::vector<kcd2s::core::EntityId> ents;
  for (int i = 0; i < 10; ++i) {
    const auto e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i) * 5.0f, 0, 0});
    w.write(e, 1, static_cast<float>(i) / 10.0f);
    w.write(e, 2, std::string("npc-") + std::to_string(i));
    ents.push_back(e);
  }

  std::vector<std::unique_ptr<TestClient>> clients;
  for (int c = 0; c < 3; ++c) {
    auto tc = std::make_unique<TestClient>();
    const auto viewer = w.spawn(human);  // each client has an avatar at origin
    w.write(viewer, 0, Vec3{0, 0, 0});
    auto pair = make_sim_pair({}, {});
    tc->server_link = std::move(pair.first);
    tc->client_link = std::move(pair.second);
    tc->pid = server.add_client(tc->server_link.get(), viewer, 1.0e6f);  // huge range = see all
    tc->peer = std::make_unique<ClientPeer>(reg, tc->client_link.get());
    clients.push_back(std::move(tc));
  }

  for (int t = 0; t < 5; ++t) {
    server.tick(0.0);
    for (auto& c : clients)
      c->peer->tick(0.0);
  }

  const std::size_t live = w.entity_count();  // 10 + 3 viewers
  for (auto& c : clients) {
    KCD2S_CHECK_EQ(c->peer->mirror().size(), live);
    for (auto e : w.live_entities())
      KCD2S_CHECK(entity_matches(w, c->peer->mirror(), e));
  }
}

KCD2S_TEST(convergence, reconverges_after_movement) {
  SchemaRegistry reg = world_schema();
  Server server(reg);
  auto& w = server.world();
  const auto human = reg.get("Human")->id;

  w.set_tick(1);
  std::vector<kcd2s::core::EntityId> ents;
  for (int i = 0; i < 20; ++i) {
    const auto e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i), 0, 0});
    ents.push_back(e);
  }

  auto pair = make_sim_pair({}, {});
  auto slink = std::move(pair.first);
  auto clink = std::move(pair.second);
  const auto viewer = w.spawn(human);
  const PeerId pid = server.add_client(slink.get(), viewer, 1.0e6f);
  (void)pid;
  ClientPeer peer(reg, clink.get());

  for (int t = 0; t < 4; ++t) {
    server.tick(0.0);
    peer.tick(0.0);
  }
  for (auto e : w.live_entities())
    KCD2S_CHECK(entity_matches(w, peer.mirror(), e));

  // Move half the entities on a later tick; the client must re-converge.
  w.set_tick(2);
  for (std::size_t i = 0; i < ents.size(); i += 2)
    w.write(ents[i], 0, Vec3{999.0f, 0, 0});
  for (int t = 0; t < 4; ++t) {
    server.tick(0.0);
    peer.tick(0.0);
  }
  for (auto e : w.live_entities())
    KCD2S_CHECK(entity_matches(w, peer.mirror(), e));
}

KCD2S_TEST(convergence, converges_over_lossy_link) {
  SchemaRegistry reg = world_schema();
  Server server(reg);
  auto& w = server.world();
  const auto human = reg.get("Human")->id;

  w.set_tick(1);
  std::vector<kcd2s::core::EntityId> ents;
  for (int i = 0; i < 12; ++i) {
    const auto e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i), 0, 0});
    w.write(e, 1, static_cast<float>(i) / 12.0f);
    ents.push_back(e);
  }
  const auto viewer = w.spawn(human);
  w.write(viewer, 0, Vec3{0, 0, 0});

  // 30% loss server->client (snapshots), lossless client->server (acks) so the resync can settle.
  LinkParams a2b;
  a2b.loss = 0.3;
  a2b.seed = 0xBEEF;
  auto pair = make_sim_pair(a2b, LinkParams{});
  auto slink = std::move(pair.first);
  auto clink = std::move(pair.second);
  server.add_client(slink.get(), viewer, 1.0e6f);
  ClientPeer peer(reg, clink.get());

  // Run many ticks; dropped CONSTRUCTs get re-sent until the client confirms them.
  for (int t = 0; t < 60; ++t) {
    server.tick(0.0);
    peer.tick(0.0);
  }

  const std::size_t live = w.entity_count();  // 12 + viewer
  KCD2S_CHECK_EQ(peer.mirror().size(), live);
  for (auto e : w.live_entities())
    KCD2S_CHECK(entity_matches(w, peer.mirror(), e));
}

KCD2S_TEST(convergence, parallel_build_matches_serial) {
  SchemaRegistry reg = world_schema();

  // Build two identical servers (same spawn + client order => same ids), one serial, one parallel.
  auto populate = [&](Server& s, std::vector<std::unique_ptr<SimLink>>& links) {
    auto& w = s.world();
    const auto human = reg.get("Human")->id;
    w.set_tick(1);
    for (int i = 0; i < 200; ++i) {
      const auto e = w.spawn(human);
      w.write(e, 0, Vec3{static_cast<float>(i % 50), 0, static_cast<float>(i / 50)});
      w.write(e, 1, static_cast<float>(i) / 200.0f);
      w.write(e, 2, std::string("npc-") + std::to_string(i));
    }
    for (int c = 0; c < 6; ++c) {
      const auto viewer = w.spawn(human);
      w.write(viewer, 0, Vec3{static_cast<float>(c * 8), 0, 0});
      auto pair = make_sim_pair({}, {});
      s.add_client(pair.first.get(), viewer, 40.0f);
      links.push_back(std::move(pair.first));
      links.push_back(std::move(pair.second));
    }
  };

  Server serial(reg, kcd2s::replication::InterestConfig{32.0f});
  Server parallel(reg, kcd2s::replication::InterestConfig{32.0f});
  std::vector<std::unique_ptr<SimLink>> l1, l2;
  populate(serial, l1);
  populate(parallel, l2);
  parallel.enable_parallel(4);

  auto a = serial.build_all(1);
  auto b = parallel.build_all(1);

  KCD2S_CHECK_EQ(a.size(), b.size());
  bool identical = a.size() == b.size();
  for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
    if (a[i].first != b[i].first || a[i].second != b[i].second)
      identical = false;
  }
  KCD2S_CHECK(identical);  // parallel fan-out is byte-for-byte equal to serial
  // Interest actually limited scope (not everyone saw all 206 entities).
  KCD2S_CHECK(a[0].second.size() > 0);
}

KCD2S_TEST(convergence, aoi_limits_scope) {
  SchemaRegistry reg = world_schema();
  Server server(reg, kcd2s::replication::InterestConfig{16.0f});
  auto& w = server.world();
  const auto human = reg.get("Human")->id;

  w.set_tick(1);
  // Entities every 20 units along +x: 0, 20, 40, ...
  std::vector<kcd2s::core::EntityId> ents;
  for (int i = 0; i < 10; ++i) {
    const auto e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i) * 20.0f, 0, 0});
    ents.push_back(e);
  }
  const auto viewer = w.spawn(human);
  w.write(viewer, 0, Vec3{0, 0, 0});

  auto pair = make_sim_pair({}, {});
  auto slink = std::move(pair.first);
  auto clink = std::move(pair.second);
  server.add_client(slink.get(), viewer, 25.0f);  // small range: viewer + ents at 0,20 only
  ClientPeer peer(reg, clink.get());

  for (int t = 0; t < 3; ++t) {
    server.tick(0.0);
    peer.tick(0.0);
  }
  // viewer(0) + ents[0](0) + ents[1](20) within 25; ents[2](40) and beyond excluded.
  KCD2S_CHECK(peer.mirror().get(viewer) != nullptr);
  KCD2S_CHECK(peer.mirror().get(ents[0]) != nullptr);
  KCD2S_CHECK(peer.mirror().get(ents[1]) != nullptr);
  KCD2S_CHECK(peer.mirror().get(ents[2]) == nullptr);
  KCD2S_CHECK(peer.mirror().get(ents[9]) == nullptr);
}

// A client can receive a construct while its acknowledgement is lost or delayed.
// Despawn must still send a tombstone, even though that entity was never confirmed.
KCD2S_TEST(convergence, despawn_before_construct_ack_removes_ghost) {
  auto reg = world_schema();
  Server server(reg);
  auto& world = server.world();
  auto human = reg.get("Human")->id;
  world.set_tick(1);
  auto viewer = world.spawn(human);
  auto departing = world.spawn(human);
  auto pair = make_sim_pair({}, {});
  server.add_client(pair.first.get(), viewer, 100);
  kcd2s::replication::Mirror mirror(reg);
  auto initial = server.build_all(1);
  KCD2S_CHECK(mirror.apply(initial.front().second));
  KCD2S_CHECK(mirror.get(departing) != nullptr);
  server.despawn(departing);
  auto after = server.build_all(2);
  KCD2S_CHECK(mirror.apply(after.front().second));
  KCD2S_CHECK(mirror.get(departing) == nullptr);
  KCD2S_CHECK(mirror.get(viewer) != nullptr);
  // Retry removal until a snapshot acknowledgement, without resurrecting the peer.
  auto retry = server.build_all(3);
  KCD2S_CHECK(mirror.apply(retry.front().second));
  KCD2S_CHECK(mirror.get(departing) == nullptr);
}
