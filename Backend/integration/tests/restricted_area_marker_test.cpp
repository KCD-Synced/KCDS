#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/restricted_area_marker.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// The server's restricted-area marker switch: RESTRICTED_AREA_MARKER reaches the
// player's own client, which hands each change over once.
KCD2S_TEST(restricted_area_marker, reaches_the_player_client_once_per_order) {
  replication::SchemaRegistry schema;
  auto type = schema.define("Player")
                  .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                         replication::Authority::Owner)
                  .id();
  Server server(schema);
  server.world().set_tick(1);
  auto e = server.world().spawn(type);
  server.world().write(e, 0, core::Vec3{});
  auto links = transport::make_loopback_pair();
  auto p = server.add_client(links.first.get(), e, 10000);
  WorldConfig c;
  KCD2S_CHECK(server.configure_world(p, c));
  server.set_owner(e, p);
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
  KCD2S_CHECK(!client.take_restricted_area_marker().has_value());

  KCD2S_CHECK(server.send_restricted_area_marker(e, false));
  KCD2S_CHECK(!server.send_restricted_area_marker(e + 1, false));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto marker = client.take_restricted_area_marker();
  KCD2S_CHECK(marker.has_value() && !*marker);
  KCD2S_CHECK(!client.take_restricted_area_marker().has_value());

  KCD2S_CHECK(server.send_restricted_area_marker(e, true));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  marker = client.take_restricted_area_marker();
  KCD2S_CHECK(marker.has_value() && *marker);
}

KCD2S_TEST(restricted_area_marker, rejects_malformed_messages) {
  bool shown = false;
  KCD2S_CHECK(decode_restricted_area_marker(encode_restricted_area_marker(true), shown));
  KCD2S_CHECK(shown);
  KCD2S_CHECK(decode_restricted_area_marker(encode_restricted_area_marker(false), shown));
  KCD2S_CHECK(!shown);
  KCD2S_CHECK(!decode_restricted_area_marker({kRestrictedAreaMarkerTag, 2}, shown));
  KCD2S_CHECK(!decode_restricted_area_marker({kRestrictedAreaMarkerTag}, shown));
  KCD2S_CHECK(!decode_restricted_area_marker({kRestrictedAreaMarkerTag, 1, 0}, shown));
  KCD2S_CHECK(
      !decode_restricted_area_marker({std::uint8_t(kRestrictedAreaMarkerTag + 1), 1}, shown));
}
