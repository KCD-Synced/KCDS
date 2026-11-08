#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/lobby.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s;
using namespace kcd2s::integration;

// LOBBY: the server holds a player out of the world and lets them in; their client hears both.

KCD2S_TEST(lobby, the_flag_survives_the_wire_and_nonsense_does_not) {
  bool held = false;
  KCD2S_CHECK(decode_lobby(encode_lobby(true), held));
  KCD2S_CHECK(held);
  KCD2S_CHECK(decode_lobby(encode_lobby(false), held));
  KCD2S_CHECK(!held);
  KCD2S_CHECK(!decode_lobby({kLobbyTag, 2}, held));
  KCD2S_CHECK(!decode_lobby({kLobbyTag}, held));
}

KCD2S_TEST(lobby, the_players_client_hears_held_and_let_in) {
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
  KCD2S_CHECK(!client.take_lobby().has_value());
  KCD2S_CHECK(server.send_lobby(player, true));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  const auto held = client.take_lobby();
  KCD2S_CHECK(held.has_value() && *held);
  KCD2S_CHECK(!client.take_lobby().has_value());
  KCD2S_CHECK(server.send_lobby(player, false));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  const auto let_in = client.take_lobby();
  KCD2S_CHECK(let_in.has_value() && !*let_in);
  KCD2S_CHECK(!server.send_lobby(player + 100, true));
}
