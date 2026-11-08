#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/level_travel.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"

#include <cmath>
#include <limits>

using namespace kcd2s;
using namespace kcd2s::integration;

// LEVEL_TRAVEL: where a player's fast travel ended reaches the server, at most once a second.

KCD2S_TEST(level_travel, the_place_survives_the_wire_and_nonsense_does_not) {
  TravelEnd end;
  KCD2S_CHECK(decode_travel_end(encode_travel_end({2644.9f, 1531.5f, 87.25f}), end));
  KCD2S_CHECK_EQ(end.x, 2644.9f);
  KCD2S_CHECK_EQ(end.y, 1531.5f);
  KCD2S_CHECK_EQ(end.z, 87.25f);
  const float nan = std::numeric_limits<float>::quiet_NaN();
  KCD2S_CHECK(!decode_travel_end(encode_travel_end({nan, 0, 0}), end));
  auto short_message = encode_travel_end({1, 2, 3});
  short_message.pop_back();
  KCD2S_CHECK(!decode_travel_end(short_message, end));
}

KCD2S_TEST(level_travel, a_ready_player_reports_one_end_a_second) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto player = server.world().spawn(type);
  server.world().write(player, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto peer = server.add_client(links.first.get(), player, 10000);
  KCD2S_CHECK(server.configure_world(peer, WorldConfig{}));
  server.set_owner(player, peer);
  ClientPeer client(schema, links.second.get());
  double now = 0;
  auto tick = [&] {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    client.tick(now);
  };
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  // Nothing before the world is ready.
  KCD2S_CHECK(!client.send_travel_end(1, 2, 3));
  KCD2S_CHECK(client.mark_world_ready(1));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(client.send_travel_end(2644.9f, 1531.5f, 87.25f));
  KCD2S_CHECK(client.send_travel_end(10, 20, 30));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  const auto ends = server.take_travel_ends();
  KCD2S_CHECK_EQ(ends.size(), std::size_t(1));
  KCD2S_CHECK(ends[0].first == player);
  KCD2S_CHECK_EQ(ends[0].second.x, 2644.9f);
  KCD2S_CHECK(server.take_travel_ends().empty());
}
