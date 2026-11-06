#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/custom_buffs.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// The resources' own buffs (Buffs.register): CUSTOM_BUFF.
namespace {

CustomBuff warmth() {
  CustomBuff buff;
  buff.kind = CustomBuff::Kind::Show;
  buff.name = "warmth";
  parse_guid("7e252c71-5e41-472c-ad32-f223a664faab", buff.icon);  // hangover
  buff.label = "Teplo \xc4\x8d";
  buff.duration_ms = 120000;
  buff.remaining_ms = 90500;
  return buff;
}

CustomBuff hidden(const std::string& name) {
  CustomBuff buff;
  buff.kind = CustomBuff::Kind::Hide;
  buff.name = name;
  return buff;
}

}  // namespace

KCD2S_TEST(custom_buffs, round_trip) {
  CustomBuff decoded;
  const auto show = encode_custom_buff(warmth());
  KCD2S_CHECK(decode_custom_buff(show, decoded) && decoded == warmth());
  CustomBuff endless = warmth();
  endless.duration_ms = -1;
  endless.remaining_ms = -1;
  KCD2S_CHECK(decode_custom_buff(encode_custom_buff(endless), decoded) && decoded == endless);
  const auto hide = encode_custom_buff(hidden("warmth"));
  KCD2S_CHECK(hide.size() == 1 + 1 + 1 + 6);
  KCD2S_CHECK(decode_custom_buff(hide, decoded) && decoded == hidden("warmth"));
  auto truncated = show;
  truncated.pop_back();
  KCD2S_CHECK(!decode_custom_buff(truncated, decoded));
  auto trailing = show;
  trailing.push_back(0);
  KCD2S_CHECK(!decode_custom_buff(trailing, decoded));
  auto unknown_kind = hide;
  unknown_kind[1] = 3;
  KCD2S_CHECK(!decode_custom_buff(unknown_kind, decoded));
}

KCD2S_TEST(custom_buffs, bounds) {
  KCD2S_CHECK(custom_buff_name_valid("my:buff_1.a-b"));
  KCD2S_CHECK(!custom_buff_name_valid(""));
  KCD2S_CHECK(!custom_buff_name_valid("with space"));
  KCD2S_CHECK(!custom_buff_name_valid(std::string(kCustomBuffNameMax + 1, 'a')));
  KCD2S_CHECK(encode_custom_buff(hidden("bad name")).empty());
  CustomBuff buff = warmth();
  buff.label = "";
  KCD2S_CHECK(encode_custom_buff(buff).empty());
  buff.label = std::string(kCustomBuffLabelMax + 1, 'a');
  KCD2S_CHECK(encode_custom_buff(buff).empty());
  buff.label = std::string("a\0b", 3);
  KCD2S_CHECK(encode_custom_buff(buff).empty());
  buff = warmth();
  buff.duration_ms = kCustomBuffDurationMaxMs + 1;
  KCD2S_CHECK(encode_custom_buff(buff).empty());
  buff.duration_ms = kCustomBuffDurationMaxMs;
  CustomBuff decoded;
  KCD2S_CHECK(decode_custom_buff(encode_custom_buff(buff), decoded) && decoded == buff);
}

// The native modifiers travel as the game's buff_params text.
KCD2S_TEST(custom_buffs, native_params) {
  CustomBuff buff = warmth();
  buff.params = "mst*1.5,cap+100,charisma-2.5";
  CustomBuff decoded;
  KCD2S_CHECK(decode_custom_buff(encode_custom_buff(buff), decoded) && decoded == buff);
  KCD2S_CHECK(decoded.params == "mst*1.5,cap+100,charisma-2.5");
  KCD2S_CHECK(custom_buff_params_valid(""));
  KCD2S_CHECK(custom_buff_params_valid("health+1/s, srg=0.5"));
  buff.params = "mst*1.5;drop";
  KCD2S_CHECK(encode_custom_buff(buff).empty());
  buff.params = std::string(kCustomBuffParamsMax + 1, 'a');
  KCD2S_CHECK(encode_custom_buff(buff).empty());
}

// A show and a hide reach the player's own client once its world is ready, in order.
KCD2S_TEST(custom_buffs, reach_the_ready_player_client_in_order) {
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
  KCD2S_CHECK(server.configure_world(p, WorldConfig{}));
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
  // Not before the player's world is ready.
  KCD2S_CHECK(!server.request_custom_buff(e, warmth()));
  KCD2S_CHECK(client.mark_world_ready(client.world_config()->revision));
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(server.request_custom_buff(e, warmth()));
  KCD2S_CHECK(server.request_custom_buff(e, hidden("warmth")));
  KCD2S_CHECK(!server.request_custom_buff(e + 1, warmth()));
  KCD2S_CHECK(!server.request_custom_buff(e, hidden("bad name")));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  const auto received = client.take_custom_buffs();
  KCD2S_CHECK(received.size() == 2);
  KCD2S_CHECK(received.size() == 2 && received[0] == warmth() && received[1] == hidden("warmth"));
  KCD2S_CHECK(client.take_custom_buffs().empty());
}
