#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/departure.hpp"
#include "kcd2s/integration/session_end.hpp"
#include "kcd2s/testkit.hpp"
#include <string>
using namespace kcd2s;
using namespace kcd2s::integration;

// The server ends a session and says why: SESSION_END reaches the player's own
// client, which hands it over once.
KCD2S_TEST(session_end, reaches_the_player_client_once) {
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
  KCD2S_CHECK(!client.take_session_end().has_value());

  KCD2S_CHECK(server.send_session_end(e, {SessionEndKind::Kicked, "Spamming the chat"}));
  KCD2S_CHECK(!server.send_session_end(e + 1, {SessionEndKind::Kicked, "nobody"}));
  KCD2S_CHECK(!server.send_session_end(e, {SessionEndKind::Kicked, std::string(201, 'x')}));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto end = client.take_session_end();
  KCD2S_CHECK(end.has_value());
  if (end) {
    KCD2S_CHECK(end->kind == SessionEndKind::Kicked);
    KCD2S_CHECK_EQ(end->reason, std::string("Spamming the chat"));
  }
  KCD2S_CHECK(!client.take_session_end().has_value());
}

KCD2S_TEST(session_end, encodes_utf8_reasons_and_rejects_malformed_messages) {
  SessionEnd end;
  const std::string reason =
      "Server se restartuje \xc4\x8d"
      "akaj";
  KCD2S_CHECK(decode_session_end(encode_session_end({SessionEndKind::Closed, reason}), end));
  KCD2S_CHECK(end.kind == SessionEndKind::Closed);
  KCD2S_CHECK_EQ(end.reason, reason);
  KCD2S_CHECK(decode_session_end(encode_session_end({SessionEndKind::Kicked, ""}), end));
  KCD2S_CHECK(end.reason.empty());
  KCD2S_CHECK(encode_session_end({SessionEndKind::Closed, std::string(200, 'a')}).size() == 204);
  KCD2S_CHECK(encode_session_end({SessionEndKind::Closed, std::string(201, 'a')}).empty());

  // A resource lets the player go (Player.disconnect), the game kept or closed.
  KCD2S_CHECK(decode_session_end(encode_session_end({SessionEndKind::Left, "Bye"}), end));
  KCD2S_CHECK(end.kind == SessionEndKind::Left);
  KCD2S_CHECK(decode_session_end(encode_session_end({SessionEndKind::Quit, ""}), end));
  KCD2S_CHECK(end.kind == SessionEndKind::Quit);

  auto message = encode_session_end({SessionEndKind::Kicked, "abc"});
  auto bad = message;
  // A ban of the server's own (Player.ban, Bans).
  KCD2S_CHECK(decode_session_end(encode_session_end({SessionEndKind::Banned, "Cheating"}), end));
  KCD2S_CHECK(end.kind == SessionEndKind::Banned);
  bad[1] = 6;
  KCD2S_CHECK(!decode_session_end(bad, end));
  bad[1] = 0;
  KCD2S_CHECK(!decode_session_end(bad, end));
  bad = message;
  bad.pop_back();
  KCD2S_CHECK(!decode_session_end(bad, end));
  bad = message;
  bad.push_back('d');
  KCD2S_CHECK(!decode_session_end(bad, end));
  KCD2S_CHECK(!decode_session_end({kSessionEndTag, 1, 0}, end));
  KCD2S_CHECK(!decode_session_end({std::uint8_t(kSessionEndTag + 1), 1, 0, 0}, end));
}

KCD2S_TEST(session_end, a_kick_departs_with_its_own_reason_name) {
  KCD2S_CHECK_EQ(std::string(departure_reason_name(DepartureReason::Kicked)),
                 std::string("kicked"));
}
