#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/client_natives.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// CLIENT_NATIVES: what a client's game build gave it, once per connection.
KCD2S_TEST(client_natives, round_trips_and_refuses_what_cannot_travel) {
  ClientNatives natives;
  natives.found = 748;
  natives.missing = {"PerfectBlockWindow", "SetCombatTarget"};
  ClientNatives decoded;
  KCD2S_CHECK(decode_client_natives(encode_client_natives(natives), decoded) && decoded == natives);
  natives.missing.clear();
  KCD2S_CHECK(decode_client_natives(encode_client_natives(natives), decoded) && decoded == natives);
  // A build without combat says so.
  natives.combat = false;
  KCD2S_CHECK(decode_client_natives(encode_client_natives(natives), decoded) && !decoded.combat &&
              decoded.riding);
  // And one without riding, apart from combat.
  natives.combat = true;
  natives.riding = false;
  KCD2S_CHECK(decode_client_natives(encode_client_natives(natives), decoded) && decoded.combat &&
              !decoded.riding);
  // And one without the inventory sync.
  natives.riding = true;
  natives.inventory = false;
  KCD2S_CHECK(decode_client_natives(encode_client_natives(natives), decoded) && decoded.combat &&
              decoded.riding && !decoded.inventory);
  auto flag = encode_client_natives(natives);
  flag[3] = 8;
  KCD2S_CHECK(!decode_client_natives(flag, decoded));
  natives.inventory = true;
  // An empty or too long name, too many names, a control character, a cut message.
  natives.missing = {""};
  KCD2S_CHECK(encode_client_natives(natives).empty());
  natives.missing = {std::string(kMaxClientNativeNameBytes + 1, 'x')};
  KCD2S_CHECK(encode_client_natives(natives).empty());
  natives.missing = std::vector<std::string>(kMaxClientNativeNames + 1, "Name");
  KCD2S_CHECK(encode_client_natives(natives).empty());
  natives.missing = {"Bad\nName"};
  KCD2S_CHECK(!decode_client_natives(encode_client_natives(natives), decoded));
  natives.missing = {"Name"};
  auto cut = encode_client_natives(natives);
  cut.pop_back();
  KCD2S_CHECK(!decode_client_natives(cut, decoded));
}

KCD2S_TEST(client_natives, the_server_takes_one_report_per_connection) {
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
  ClientNatives natives;
  natives.found = 700;
  natives.missing = {"SetCombatTarget"};
  // Nothing before the world is ready.
  KCD2S_CHECK(!client.send_client_natives(natives));
  KCD2S_CHECK(client.mark_world_ready(1));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(client.send_client_natives(natives));
  natives.found = 1;
  KCD2S_CHECK(client.send_client_natives(natives));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  const auto reports = server.take_client_natives();
  KCD2S_CHECK_EQ(reports.size(), std::size_t(1));
  if (reports.size() == 1) {
    KCD2S_CHECK(reports[0].first == e);
    KCD2S_CHECK_EQ(reports[0].second.found, std::uint32_t(700));
    KCD2S_CHECK(reports[0].second.missing == std::vector<std::string>{"SetCombatTarget"});
  }
}
