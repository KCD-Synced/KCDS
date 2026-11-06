#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "kcd2s/testkit.hpp"

using namespace kcd2s;
using namespace kcd2s::integration;

namespace {
struct ScriptPair {
  replication::SchemaRegistry schema;
  std::unique_ptr<Server> server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  std::unique_ptr<ClientPeer> client;
  core::PeerId peer{};
  core::EntityId avatar{};
  double now = 0;

  ScriptPair() {
    const auto type = schema.define("Player")
                          .field("position", replication::FieldKind::Vec3,
                                 replication::Priority::High, replication::Authority::Owner)
                          .id();
    server = std::make_unique<Server>(schema);
    avatar = server->world().spawn(type);
    server->world().write(avatar, 0, core::Vec3{});
    transport::LinkParams params;
    params.loss = .15;
    params.duplicate = .3;
    params.min_delay = 1;
    params.max_delay = 3;
    links = transport::make_sim_pair(params, params);
    peer = server->add_client(links.first.get(), avatar, 1000);
    server->set_owner(avatar, peer);
    server->configure_world(peer, {});
    client = std::make_unique<ClientPeer>(schema, links.second.get());
    pump(240);
  }

  void pump(unsigned count) {
    for (unsigned i = 0; i < count; ++i) {
      now += 1.0 / 60;
      server->world().set_tick(server->world().tick() + 1);
      links.first->poll();
      links.second->poll();
      server->tick(now);
      client->tick(now);
    }
  }
};
}  // namespace

KCD2S_TEST(script_channel, downloads_before_world_ready_and_stamps_actual_sender) {
  ScriptPair pair;
  sdk::Value request(sdk::Value::Map{{"op", sdk::Value("catalogRequest")},
                                     {"player", sdk::Value(std::int64_t{999})}});
  KCD2S_CHECK(!pair.client->world_ready());
  KCD2S_CHECK(pair.client->send_script(request));
  pair.pump(240);
  auto messages = pair.server->take_scripts();
  KCD2S_CHECK_EQ(messages.size(), std::size_t{1});
  KCD2S_CHECK_EQ(messages[0].peer, pair.peer);
  KCD2S_CHECK_EQ(messages[0].player, pair.avatar);
  KCD2S_CHECK_EQ(script_operation(messages[0].message), std::string("catalogRequest"));
  KCD2S_CHECK(!pair.client->send_script(
      sdk::Value(sdk::Value::Map{{"op", sdk::Value("ensure")}, {"name", sdk::Value("mode")}})));
}

KCD2S_TEST(script_channel, large_catalog_survives_loss_reordering_and_duplication_once) {
  ScriptPair pair;
  const std::string source(50000, 'a');
  sdk::Value value(
      sdk::Value::Map{{"op", sdk::Value("catalogData")}, {"data", sdk::Value(source)}});
  KCD2S_CHECK(pair.server->send_script(pair.peer, value));
  pair.pump(600);
  auto messages = pair.client->take_scripts();
  KCD2S_CHECK_EQ(messages.size(), std::size_t{1});
  KCD2S_CHECK_EQ(script_field(messages[0], "data")->as_str(), source);
  pair.pump(240);
  KCD2S_CHECK(pair.client->take_scripts().empty());
  KCD2S_CHECK(!pair.server->send_script(
      pair.peer, sdk::Value(sdk::Value::Map{{"op", sdk::Value("catalogData")},
                                            {"data", sdk::Value(std::string(65536, 'b'))}})));
}

KCD2S_TEST(script_channel, stale_world_revision_and_duplicate_operation_are_rejected) {
  auto links = transport::make_loopback_pair();
  transport::Connection connection(links.first.get());
  connection.set_channel_payload_budget(kScriptChannel, 4096);
  std::uint32_t sequence = 0;
  const sdk::Value value(sdk::Value::Map{{"op", sdk::Value("event")}});
  KCD2S_CHECK(queue_script(connection, 1, sequence, value));
  connection.flush(0);
  transport::Connection receiver(links.second.get());
  const auto messages = receiver.receive(0);
  KCD2S_CHECK_EQ(messages.size(), std::size_t{1});
  ScriptReceiver decoder;
  KCD2S_CHECK(!decoder.receive(messages[0].bytes, 2));
  KCD2S_CHECK(decoder.receive(messages[0].bytes, 1).has_value());
  const sdk::Value duplicate(
      sdk::Value::Map{{"op", sdk::Value("event")}, {"op", sdk::Value("ensure")}});
  KCD2S_CHECK(!queue_script(connection, 1, sequence, duplicate));
  auto truncated = messages[0].bytes;
  truncated.pop_back();
  KCD2S_CHECK(!decoder.receive(truncated, 1));
}

KCD2S_TEST(script_channel, configured_channel_budget_paces_bursts_without_losing_messages) {
  auto links = transport::make_loopback_pair();
  transport::Connection sender(links.first.get()), receiver(links.second.get());
  sender.set_channel_payload_budget(kScriptChannel, 1000);
  for (unsigned i = 0; i < 20; ++i)
    sender.queue(kScriptChannel, std::vector<std::uint8_t>(850, std::uint8_t(i)));
  unsigned received = 0;
  for (unsigned tick = 0; tick < 100; ++tick) {
    const auto now = double(tick) / 60;
    sender.flush(now);
    auto messages = receiver.receive(now);
    KCD2S_CHECK(messages.size() <= 1);
    for (const auto& message : messages) {
      KCD2S_CHECK_EQ(message.bytes[0], std::uint8_t(received));
      ++received;
    }
    receiver.flush(now);
    sender.receive(now);
  }
  KCD2S_CHECK_EQ(received, 20u);
}

KCD2S_TEST(script_channel, client_transform_moves_only_the_own_player_and_is_rate_limited) {
  ScriptPair pair;
  KCD2S_CHECK(pair.client->world_config().has_value());
  KCD2S_CHECK(pair.client->mark_world_ready(pair.client->world_config()->revision));
  pair.pump(60);
  auto message = [&](core::EntityId id, const char* field, sdk::Value x) {
    return sdk::Value(sdk::Value::Map{{"op", sdk::Value("entityTransform")},
                                      {"id", sdk::Value(std::int64_t(id))},
                                      {"field", sdk::Value(field)},
                                      {"x", std::move(x)},
                                      {"y", sdk::Value(2.0)},
                                      {"z", sdk::Value(std::int64_t{3})}});
  };
  auto request = [&](core::EntityId id, double x) {
    return message(id, "position", sdk::Value(x));
  };
  auto position = [&](core::EntityId id) {
    return std::get<core::Vec3>(*pair.server->world().read(id, 0));
  };

  // The own player moves through the authoritative force path and the owner is told.
  KCD2S_CHECK(pair.client->send_script(request(pair.avatar, 1.5)));
  pair.pump(60);
  KCD2S_CHECK((position(pair.avatar) == core::Vec3{1.5f, 2, 3}));
  KCD2S_CHECK(!pair.client->take_forced_transforms().empty());

  // Another entity, a malformed value and an unknown field are ignored.
  const auto other = pair.server->world().spawn(pair.server->world().slot(pair.avatar)->type);
  pair.server->world().write(other, 0, core::Vec3{});
  KCD2S_CHECK(pair.client->send_script(request(other, 9)));
  KCD2S_CHECK(pair.client->send_script(message(pair.avatar, "position", sdk::Value("4"))));
  KCD2S_CHECK(pair.client->send_script(message(pair.avatar, "scale", sdk::Value(4.0))));
  pair.pump(60);
  KCD2S_CHECK((position(other) == core::Vec3{}));
  KCD2S_CHECK((position(pair.avatar) == core::Vec3{1.5f, 2, 3}));

  // At most ten requests per second are applied; the rest of the burst is dropped.
  pair.pump(70);
  for (int i = 0; i < 14; ++i)
    KCD2S_CHECK(pair.client->send_script(request(pair.avatar, 100 + i)));
  pair.pump(30);
  KCD2S_CHECK((position(pair.avatar) == core::Vec3{109, 2, 3}));
}

KCD2S_TEST(script_channel, gameplay_ready_counts_only_after_world_ready_of_the_same_revision) {
  ScriptPair pair;
  KCD2S_CHECK(pair.client->world_config().has_value());
  const auto revision = pair.client->world_config()->revision;
  // The loading screen cannot come down before the world is ready.
  KCD2S_CHECK(!pair.client->mark_gameplay_started(revision));
  KCD2S_CHECK(pair.client->mark_world_ready(revision));
  KCD2S_CHECK(!pair.client->mark_gameplay_started(revision + 1));
  pair.pump(60);
  KCD2S_CHECK(pair.server->player_ready(pair.avatar));
  KCD2S_CHECK(!pair.server->player_gameplay_ready(pair.avatar));
  KCD2S_CHECK(pair.client->mark_gameplay_started(revision));
  KCD2S_CHECK(pair.client->gameplay_started());
  // Reported once per revision.
  KCD2S_CHECK(!pair.client->mark_gameplay_started(revision));
  pair.pump(60);
  KCD2S_CHECK(pair.server->player_gameplay_ready(pair.avatar));
  KCD2S_CHECK(!pair.server->player_gameplay_ready(pair.avatar + 1));
  // A forged or out-of-order report is ignored.
  std::uint32_t decoded = 0;
  KCD2S_CHECK(integration::decode_gameplay_ready(integration::encode_gameplay_ready(5), decoded));
  KCD2S_CHECK_EQ(decoded, 5u);
  KCD2S_CHECK(!integration::decode_gameplay_ready(integration::encode_world_ready(5), decoded));
  KCD2S_CHECK(!integration::decode_gameplay_ready(integration::encode_gameplay_ready(0), decoded));
}
