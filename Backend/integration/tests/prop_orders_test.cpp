// The props and effects a server places reach a client's game whole: a burst of more
// orders than came before in one frame is kept, not cut at 512.
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/placed_objects.hpp"
#include "kcd2s/integration/server.hpp"
#include "../local/movement.hpp"
#include "kcd2s/testkit.hpp"

#include <memory>
#include <string>
#include <utility>

using namespace kcd2s;
using namespace kcd2s::integration;

namespace {

// A server and one player's client over a lossless link, the client's world ready.
struct OnePlayer {
  replication::SchemaRegistry schema = local::player_schema();
  Server server{schema};
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  std::unique_ptr<ClientPeer> client;
  core::EntityId player{};
  double now{};

  OnePlayer() {
    links = transport::make_sim_pair({}, {});
    player = local::spawn_player(server.world());
    const core::PeerId id = server.add_client(links.first.get(), player, 1000);
    server.set_owner(player, id);
    server.configure_world(id, {});
    client = std::make_unique<ClientPeer>(schema, links.second.get());
    pump(30);
    client->mark_world_ready(1);
    pump(30);
  }

  void pump(int ticks) {
    for (int i = 0; i < ticks; ++i) {
      now += 1. / 60;
      server.world().set_tick(server.world().tick() + 1);
      links.first->poll();
      links.second->poll();
      server.tick(now);
      client->tick(now);
    }
  }
};

PropOrder barrel(std::uint32_t id) {
  PropOrder order;
  order.kind = PropKind::Spawn;
  order.id = id;
  order.model = "objects/manmade/common_furniture/barrels/barrel_a.cgf";
  order.position = {float(id % 30), float(id / 30), 0.0f};
  order.rotation = {0.0f, 0.0f, 0.0f, 1.0f};
  order.scale = {1.0f, 1.0f, 1.0f};
  order.physics = PropPhysics::Static;
  return order;
}

}  // namespace

KCD2S_TEST(prop_orders, a_burst_of_six_hundred_props_reaches_the_game_whole) {
  OnePlayer w;
  constexpr std::uint32_t kProps = 600;
  for (std::uint32_t id = 1; id <= kProps; ++id) {
    KCD2S_CHECK(w.server.send_prop(w.player, barrel(id)));
  }
  // A second of server ticks; the game takes the orders only then, the whole burst at once.
  w.pump(60);
  const auto orders = w.client->take_prop_orders();
  KCD2S_CHECK_EQ(orders.size(), std::size_t(kProps));
  KCD2S_CHECK_EQ(w.client->dropped_placed_orders(), std::uint64_t(0));
  KCD2S_CHECK_EQ(orders.front().id, 1u);
  KCD2S_CHECK_EQ(orders.back().id, kProps);
}
