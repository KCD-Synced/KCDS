#include "kcd2s/integration/server.hpp"
#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/knockout.hpp"
#include "kcd2s/integration/control.hpp"
#include "kcd2s/wire/packet.hpp"
#include "kcd2s/testkit.hpp"
using namespace kcd2s;
using namespace kcd2s::integration;

// Knock-out instead of death: KNOCKED_OUT reaches the server once per knock-out,
// REVIVE and KNOCKOUT_VISION reach the player's own client.
KCD2S_TEST(knockout, report_revive_and_vision_round_trip) {
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
  // Nothing is reported before the world is ready.
  KCD2S_CHECK(!client.send_knocked_out());
  KCD2S_CHECK(client.mark_world_ready(1));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  // Not knocked out yet: no revival is sent.
  const auto initial_epoch = server.world().slot(e)->stateEpoch;
  KCD2S_CHECK(!server.revive(e));
  KCD2S_CHECK(server.world().slot(e)->stateEpoch == initial_epoch);
  KCD2S_CHECK(!server.revive(e + 1));
  KCD2S_CHECK(server.world().slot(e)->stateEpoch == initial_epoch);

  KCD2S_CHECK(client.send_knocked_out());
  KCD2S_CHECK(client.send_knocked_out());
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  auto knockouts = server.take_knockouts();
  KCD2S_CHECK_EQ(knockouts.size(), std::size_t(1));
  KCD2S_CHECK(knockouts[0] == e);

  KCD2S_CHECK(server.revive(e));
  const auto revived_epoch = server.world().slot(e)->stateEpoch;
  KCD2S_CHECK(revived_epoch == std::uint8_t(initial_epoch + 1));
  KCD2S_CHECK(!server.revive(e));
  KCD2S_CHECK(server.world().slot(e)->stateEpoch == revived_epoch);
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK_EQ(client.take_revives(), std::size_t(1));
  KCD2S_CHECK_EQ(client.take_revives(), std::size_t(0));

  // The world from the ground, dimmed, until a script sets another view.
  KCD2S_CHECK_EQ(unsigned(client.knockout_vision()), unsigned(kKnockoutVisionDefault));
  KCD2S_CHECK(server.set_knockout_vision(e, 25));
  KCD2S_CHECK(!server.set_knockout_vision(e, 101));
  KCD2S_CHECK(!server.set_knockout_vision(e + 1, 25));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK_EQ(unsigned(client.knockout_vision()), 25u);
}

KCD2S_TEST(knockout, force_epoch_reaches_unscoped_owner_and_preserves_observer_visibility) {
  replication::SchemaRegistry schema;
  const auto type = schema.define("Player")
                        .field("position", replication::FieldKind::Vec3,
                               replication::Priority::High, replication::Authority::Owner)
                        .id();
  Server server(schema);
  server.world().set_tick(1);
  const auto player = server.world().spawn(type);
  const auto viewer = server.world().spawn(type);
  server.world().write(player, 0, core::Vec3{});
  server.world().write(viewer, 0, core::Vec3{});
  KCD2S_CHECK(server.set_dimension(player, 1));
  KCD2S_CHECK(server.set_dimension(viewer, 2));
  auto owner_links = transport::make_loopback_pair();
  auto observer_links = transport::make_loopback_pair();
  const auto owner_id = server.add_client(owner_links.first.get(), player, 10000);
  const auto observer_id = server.add_client(observer_links.first.get(), viewer, 10000);
  KCD2S_CHECK(server.configure_world(owner_id, {}));
  KCD2S_CHECK(server.configure_world(observer_id, {}));
  server.set_owner(player, owner_id);
  ClientPeer owner(schema, owner_links.second.get());
  transport::Connection observer(observer_links.second.get());
  replication::Mirror observer_mirror(schema);
  bool observer_received_fence = false;
  double now = 0;
  const auto tick = [&]() {
    now += 1.0 / 60;
    server.world().set_tick(server.world().tick() + 1);
    server.tick(now);
    owner.tick(now);
    for (const auto& message : observer.receive(now)) {
      if (message.channel == static_cast<std::uint8_t>(wire::Channel::Snapshot)) {
        observer_mirror.apply(message.bytes);
      } else if (message.channel == static_cast<std::uint8_t>(wire::Channel::Control)) {
        core::EntityId entity = 0;
        std::uint8_t epoch = 0;
        if (decode_force_state(message.bytes, entity, epoch) && entity == player) {
          observer_received_fence = true;
        }
      }
    }
    observer.flush(now);
  };
  // No snapshot has been sent yet: the owner's SET_OWNER carries the previous epoch.
  server.force_state(player);
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(owner.mark_world_ready(1));
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(!observer_mirror.get(player));
  KCD2S_CHECK(!observer_received_fence);
  const core::Vec3 moved{2, 3, 4};
  owner.send_input(player, {{core::FieldId(0), moved}});
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(server.world().slot(player)->position == moved);
  // A previously constructed observer also loses the fence immediately when scope changes,
  // before the next snapshot has removed its old replica.
  KCD2S_CHECK(server.set_dimension(viewer, 1));
  for (int i = 0; i < 8; ++i) {
    tick();
  }
  KCD2S_CHECK(observer_mirror.get(player));
  KCD2S_CHECK(server.set_dimension(viewer, 2));
  server.force_state(player);
  for (int i = 0; i < 5; ++i) {
    tick();
  }
  KCD2S_CHECK(!observer_received_fence);
}

KCD2S_TEST(knockout, rejects_malformed_messages) {
  std::uint32_t revision = 0;
  KCD2S_CHECK(decode_knocked_out(encode_knocked_out(3), revision));
  KCD2S_CHECK_EQ(revision, 3u);
  KCD2S_CHECK(!decode_knocked_out(encode_knocked_out(0), revision));
  KCD2S_CHECK(!decode_knocked_out({kKnockedOutTag, 1, 0, 0}, revision));

  core::EntityId player = 0;
  KCD2S_CHECK(decode_revive(encode_revive(7), player));
  KCD2S_CHECK(player == core::EntityId(7));
  KCD2S_CHECK(!decode_revive(encode_revive(0), player));

  std::uint8_t percent = 0;
  KCD2S_CHECK(decode_knockout_vision(encode_knockout_vision(100), percent));
  KCD2S_CHECK_EQ(unsigned(percent), 100u);
  KCD2S_CHECK(!decode_knockout_vision({kKnockoutVisionTag, 101}, percent));
  KCD2S_CHECK(!decode_knockout_vision({kKnockoutVisionTag}, percent));
  KCD2S_CHECK(!decode_knockout_vision({kReviveTag, 10}, percent));
}
