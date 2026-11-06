#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/testkit.hpp"
#include <stdexcept>
using namespace kcd2s;
using namespace kcd2s::integration;
namespace {
struct ChatPair {
  replication::SchemaRegistry schema;
  std::unique_ptr<Server> server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  std::unique_ptr<ClientPeer> client;
  core::EntityId avatar{};
  double now = 0;
  ChatPair() {
    auto type = schema.define("Player")
                    .field("position", replication::FieldKind::Vec3, replication::Priority::High,
                           replication::Authority::Owner)
                    .id();
    server = std::make_unique<Server>(schema);
    avatar = server->world().spawn(type);
    server->world().write(avatar, 0, core::Vec3{});
    transport::LinkParams p;
    p.loss = .15;
    p.duplicate = .3;
    p.min_delay = 1;
    p.max_delay = 3;
    links = transport::make_sim_pair(p, p);
    auto peer = server->add_client(links.first.get(), avatar, 1000);
    server->set_owner(avatar, peer);
    server->configure_world(peer, {});
    client = std::make_unique<ClientPeer>(schema, links.second.get());
    pump(240);
  }
  void pump(int count) {
    for (int i = 0; i < count; ++i) {
      now += 1.0 / 60;
      server->world().set_tick(server->world().tick() + 1);
      links.first->poll();
      links.second->poll();
      server->tick(now);
      client->tick(now);
    }
  }
  void ready() {
    if (!client->mark_world_ready(1))
      throw std::runtime_error("world not ready");
    pump(240);
  }
};
}  // namespace
KCD2S_TEST(chat, validates_utf8_bounds_and_wire_direction) {
  ChatSubmission source{7, "/Horse  hello"}, decoded;
  std::uint32_t revision = 0;
  auto bytes = encode_chat_submission(3, source);
  KCD2S_CHECK(decode_chat_submission(bytes, revision, decoded));
  KCD2S_CHECK(revision == 3 && decoded.player == 7 && decoded.text == source.text);
  bytes.push_back(0);
  KCD2S_CHECK(!decode_chat_submission(bytes, revision, decoded));
  ChatMessage message{"Henry", "hello", 0xffd24dff}, result;
  auto outbound = encode_chat_message(3, message);
  KCD2S_CHECK(decode_chat_message(outbound, revision, result));
  KCD2S_CHECK(result.color == message.color);
  KCD2S_CHECK(!decode_chat_submission(outbound, revision, decoded));
  KCD2S_CHECK(!valid_chat_text(std::string(1025, 'a')));
  KCD2S_CHECK(!valid_chat_text(std::string("a\0b", 3)));
  KCD2S_CHECK(!valid_chat_text("\xc0\x80"));
  KCD2S_CHECK(!valid_chat_text("\xed\xa0\x80"));
  KCD2S_CHECK(
      valid_chat_text("\xc4\x8d"
                      "au"));
}
KCD2S_TEST(chat, owner_ready_gates_and_reliable_once) {
  ChatPair p;
  KCD2S_CHECK(!p.client->send_chat(p.avatar, "before ready"));
  p.ready();
  KCD2S_CHECK(!p.client->send_chat(p.avatar + 65536, "forged"));
  KCD2S_CHECK(p.client->send_chat(p.avatar, "/Horse  one"));
  p.pump(240);
  auto inbox = p.server->take_chat();
  KCD2S_CHECK(inbox.size() == 1);
  KCD2S_CHECK(inbox[0].text == "/Horse  one");
  p.pump(120);
  KCD2S_CHECK(p.server->take_chat().empty());
  p.server->send_chat(p.avatar, {"Server", "created", 0});
  p.pump(240);
  auto messages = p.client->take_chat();
  KCD2S_CHECK(messages.size() == 1);
  KCD2S_CHECK(messages[0].text == "created");
  p.pump(120);
  KCD2S_CHECK(p.client->take_chat().empty());
}
