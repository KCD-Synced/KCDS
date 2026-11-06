#include "kcd2s/integration/client_bridge_driver.hpp"

#include <algorithm>
#include <memory>
#include <vector>

#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/replication/schema.hpp"
#include "kcd2s/testkit.hpp"
#include "kcd2s/transport/datagram.hpp"

using namespace kcd2s::integration;
using kcd2s::bridge::TransformApply;
using kcd2s::core::EntityId;
using kcd2s::core::Vec3;
using kcd2s::replication::FieldKind;
using kcd2s::replication::Priority;
using kcd2s::replication::SchemaRegistry;
using kcd2s::transport::make_sim_pair;

// Records what the driver would do to the engine.
struct SpySink : ITransformSink {
  std::vector<EntityId> bound;
  std::vector<EntityId> unbound;
  std::vector<TransformApply> last_apply;
  void on_bind(EntityId e) override {
    bound.push_back(e);
  }
  void on_unbind(EntityId e) override {
    unbound.push_back(e);
  }
  void apply(const std::vector<TransformApply>& t) override {
    last_apply = t;
  }
  bool was_bound(EntityId e) const {
    return std::find(bound.begin(), bound.end(), e) != bound.end();
  }
  bool was_unbound(EntityId e) const {
    return std::find(unbound.begin(), unbound.end(), e) != unbound.end();
  }
};

static SchemaRegistry schema() {
  SchemaRegistry reg;
  reg.define("Human")
      .field("position", FieldKind::Vec3, Priority::High)
      .field("health", FieldKind::F32, Priority::Med);
  return reg;
}

KCD2S_TEST(client_bridge, binds_and_applies_end_to_end) {
  SchemaRegistry reg = schema();
  Server server(reg);
  auto& w = server.world();
  const auto human = reg.get("Human")->id;

  w.set_tick(1);
  std::vector<EntityId> ents;
  for (int i = 0; i < 4; ++i) {
    const auto e = w.spawn(human);
    w.write(e, 0, Vec3{static_cast<float>(i), 0, 0});
    ents.push_back(e);
  }
  const auto viewer = w.spawn(human);
  w.write(viewer, 0, Vec3{0, 0, 0});

  auto pair = make_sim_pair({}, {});
  auto slink = std::move(pair.first);
  auto clink = std::move(pair.second);
  server.add_client(slink.get(), viewer, 1.0e6f);
  ClientPeer peer(reg, clink.get());

  SpySink sink;
  ClientBridgeDriver driver(reg, sink);

  for (int t = 0; t < 3; ++t) {
    server.tick(0.0);
    peer.tick(0.0);
    driver.sync(peer.mirror());
  }

  // Every world entity (4 + viewer) got bound exactly once and shows up in the transform set.
  KCD2S_CHECK_EQ(sink.bound.size(), std::size_t{5});
  for (auto e : ents)
    KCD2S_CHECK(sink.was_bound(e));
  KCD2S_CHECK(sink.was_bound(viewer));
  KCD2S_CHECK_EQ(driver.tracked(), std::size_t{5});
  KCD2S_CHECK_EQ(sink.last_apply.size(), std::size_t{5});  // all have position
  KCD2S_CHECK(sink.unbound.empty());
}

KCD2S_TEST(client_bridge, unbinds_on_destruct) {
  SchemaRegistry reg = schema();
  Server server(reg, kcd2s::replication::InterestConfig{16.0f});
  auto& w = server.world();
  const auto human = reg.get("Human")->id;

  w.set_tick(1);
  const auto near_e = w.spawn(human);
  w.write(near_e, 0, Vec3{5, 0, 0});  // within range
  const auto viewer = w.spawn(human);
  w.write(viewer, 0, Vec3{0, 0, 0});

  auto pair = make_sim_pair({}, {});
  auto slink = std::move(pair.first);
  auto clink = std::move(pair.second);
  server.add_client(slink.get(), viewer, 20.0f);
  ClientPeer peer(reg, clink.get());

  SpySink sink;
  ClientBridgeDriver driver(reg, sink);
  for (int t = 0; t < 3; ++t) {
    server.tick(0.0);
    peer.tick(0.0);
    driver.sync(peer.mirror());
  }
  KCD2S_CHECK(sink.was_bound(near_e));

  // Move it far out of the viewer's range -> server DESTRUCTs it -> driver unbinds.
  w.set_tick(2);
  w.write(near_e, 0, Vec3{10000, 0, 0});
  for (int t = 0; t < 3; ++t) {
    server.tick(0.0);
    peer.tick(0.0);
    driver.sync(peer.mirror());
  }
  KCD2S_CHECK(sink.was_unbound(near_e));
  KCD2S_CHECK(peer.mirror().get(near_e) == nullptr);
}
