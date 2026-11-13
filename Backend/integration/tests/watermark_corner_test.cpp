#include "kcd2s/integration/watermark_corner.hpp"

#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// The server's watermark corner: WATERMARK_CORNER reaches the player's own client, which hands
// each change over once.
KCD2S_TEST(watermark_corner, reaches_the_player_client_once_per_order) {
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
  KCD2S_CHECK(!client.take_watermark_corner().has_value());

  KCD2S_CHECK(server.send_watermark_corner(e, WatermarkCorner::TopRight));
  KCD2S_CHECK(!server.send_watermark_corner(e + 1, WatermarkCorner::TopRight));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto corner = client.take_watermark_corner();
  KCD2S_CHECK(corner.has_value() && *corner == WatermarkCorner::TopRight);
  KCD2S_CHECK(!client.take_watermark_corner().has_value());
}

KCD2S_TEST(watermark_corner, rejects_malformed_messages) {
  WatermarkCorner corner = WatermarkCorner::BottomLeft;
  for (const WatermarkCorner each : {WatermarkCorner::BottomLeft, WatermarkCorner::BottomRight,
                                     WatermarkCorner::TopRight, WatermarkCorner::TopLeft}) {
    KCD2S_CHECK(decode_watermark_corner(encode_watermark_corner(each), corner));
    KCD2S_CHECK(corner == each);
  }
  KCD2S_CHECK(!decode_watermark_corner({kWatermarkCornerTag, 4}, corner));
  KCD2S_CHECK(!decode_watermark_corner({kWatermarkCornerTag}, corner));
  KCD2S_CHECK(!decode_watermark_corner({kWatermarkCornerTag, 1, 0}, corner));
  KCD2S_CHECK(!decode_watermark_corner({std::uint8_t(kWatermarkCornerTag + 1), 1}, corner));
}
