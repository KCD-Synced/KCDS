#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/world_config.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

KCD2S_TEST(world_config, server_map_and_ready_gate) {
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
  c.map = "klaster";
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
  for (int i = 0; i < 8; ++i)
    tick();
  KCD2S_CHECK(client.world_config().has_value());
  KCD2S_CHECK_EQ(client.world_config()->map, std::string("klaster"));
  KCD2S_CHECK(client.world_config()->debug);
  KCD2S_CHECK(!client.world_config()->quests);
  client.send_input(e, {{0, core::Vec3{7, 8, 9}}});
  for (int i = 0; i < 5; ++i)
    tick();
  KCD2S_CHECK(std::get<core::Vec3>(*server.world().read(e, 0)) == core::Vec3{});
  KCD2S_CHECK(!client.mark_world_ready(99));
  KCD2S_CHECK(client.mark_world_ready(1));
  for (int i = 0; i < 5; ++i)
    tick();
  client.send_input(e, {{0, core::Vec3{7, 8, 9}}});
  for (int i = 0; i < 5; ++i)
    tick();
  auto pos = std::get<core::Vec3>(*server.world().read(e, 0));
  KCD2S_CHECK_EQ(pos.x, 7.0f);
  c.map = "trosecko";
  c.revision = 2;
  KCD2S_CHECK(server.configure_world(p, c));
  for (int i = 0; i < 5; ++i)
    tick();
  KCD2S_CHECK(!client.world_ready());
  KCD2S_CHECK(!client.mark_world_ready(1));
  KCD2S_CHECK_EQ(client.world_config()->map, std::string("trosecko"));
}
KCD2S_TEST(world_config, rejects_unsafe_or_malformed_configuration) {
  WorldConfig c;
  auto good = encode_world_config(c);
  WorldConfig out;
  KCD2S_CHECK(decode_world_config(good, out));
  for (std::size_t i = 0; i < good.size(); ++i) {
    std::vector<std::uint8_t> prefix(good.begin(), good.begin() + i);
    KCD2S_CHECK(!decode_world_config(prefix, out));
  }
  c.map = "trosecko;quit";
  KCD2S_CHECK(encode_world_config(c).empty());
  c.map = "trosecko";
  c.quests = true;
  KCD2S_CHECK(encode_world_config(c).empty());
  c.quests = false;
  c.debug = false;
  KCD2S_CHECK(encode_world_config(c).empty());
}
KCD2S_TEST(world_config, carries_whether_the_map_is_revealed) {
  WorldConfig c;
  WorldConfig out;
  KCD2S_CHECK(decode_world_config(encode_world_config(c), out));
  KCD2S_CHECK(out.reveal_map);
  c.reveal_map = false;
  auto unrevealed = encode_world_config(c);
  KCD2S_CHECK(decode_world_config(unrevealed, out));
  KCD2S_CHECK(!out.reveal_map);
  unrevealed[5] = 8;
  KCD2S_CHECK(!decode_world_config(unrevealed, out));
}
KCD2S_TEST(world_config, carries_whether_the_level_people_live) {
  WorldConfig c;
  WorldConfig out;
  KCD2S_CHECK(decode_world_config(encode_world_config(c), out));
  KCD2S_CHECK(!out.level_people);
  c.level_people = true;
  c.reveal_map = false;
  KCD2S_CHECK(decode_world_config(encode_world_config(c), out));
  KCD2S_CHECK(out.level_people);
  KCD2S_CHECK(!out.reveal_map);
}
