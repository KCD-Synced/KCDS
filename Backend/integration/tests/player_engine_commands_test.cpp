#include "kcd2s/integration/client_peer.hpp"
#include "kcd2s/integration/control.hpp"
#include "kcd2s/integration/player_engine_commands.hpp"
#include "kcd2s/integration/server.hpp"
#include "kcd2s/testkit.hpp"

#include <memory>
#include <stdexcept>
#include <utility>
#include <variant>

namespace {

using namespace kcd2s;
using namespace kcd2s::integration;

struct Fixture {
  replication::SchemaRegistry schema;
  std::unique_ptr<Server> server;
  std::pair<std::unique_ptr<transport::SimLink>, std::unique_ptr<transport::SimLink>> links;
  std::unique_ptr<ClientPeer> client;
  core::EntityId owner = 0;
  core::PeerId peer = 0;
  double now = 0;

  Fixture() {
    const auto type = schema.define("Player")
                          .field("position", replication::FieldKind::Vec3,
                                 replication::Priority::High, replication::Authority::Owner)
                          .id();
    server = std::make_unique<Server>(schema);
    owner = server->world().spawn(type);
    server->world().write(owner, 0, core::Vec3{});
    links = transport::make_loopback_pair();
    peer = server->add_client(links.first.get(), owner, 10000);
    server->configure_world(peer, {});
    server->set_owner(owner, peer);
    client = std::make_unique<ClientPeer>(schema, links.second.get());
    pump();
    if (!client->mark_world_ready(1)) {
      throw std::runtime_error("fixture world not ready");
    }
    pump();
  }

  void pump(unsigned ticks = 8) {
    for (unsigned index = 0; index < ticks; ++index) {
      now += 1.0 / 60;
      server->world().set_tick(server->world().tick() + 1);
      server->tick(now);
      client->tick(now);
    }
  }

  PlayerEngineScope scope(std::uint64_t incarnation = 1, std::uint32_t revision = 1) const {
    return {revision, owner, incarnation};
  }

  void queue_stat_revive_stat(kcd2s::testkit::Context& kcd2s_ctx) {
    KCD2S_CHECK(server->request_stat(owner, {0, 5}));
    KCD2S_CHECK(client->send_knocked_out());
    pump();
    KCD2S_CHECK_EQ(server->take_knockouts().size(), std::size_t(1));
    KCD2S_CHECK(server->revive(owner));
    KCD2S_CHECK(server->request_stat(owner, {0, 80}));
    pump();
  }
};

void check_stat(kcd2s::testkit::Context& kcd2s_ctx, const PlayerEngineCommandPoll& poll,
                const PlayerEngineScope& scope, float value) {
  KCD2S_CHECK(poll.status == PlayerEngineCommandStatus::Command);
  KCD2S_CHECK(poll.command.has_value());
  if (!poll.command) {
    return;
  }
  KCD2S_CHECK(poll.command->scope == scope);
  const auto* request = std::get_if<StatRequest>(&poll.command->payload);
  KCD2S_CHECK(request && request->stat == 0 && request->value == value);
}

}  // namespace

KCD2S_TEST(player_engine_commands, first_bind_adopts_control_receipt_order_without_reconstruction) {
  Fixture fixture;
  const auto original_epoch = fixture.server->world().slot(fixture.owner)->stateEpoch;
  fixture.queue_stat_revive_stat(kcd2s_ctx);
  const auto scope = fixture.scope();

  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  const auto old_stat = fixture.client->take_player_engine_command(scope);
  const auto revive = fixture.client->take_player_engine_command(scope);
  const auto new_stat = fixture.client->take_player_engine_command(scope);
  check_stat(kcd2s_ctx, old_stat, scope, 5);
  check_stat(kcd2s_ctx, new_stat, scope, 80);
  KCD2S_CHECK(revive.command.has_value());
  if (old_stat.command && revive.command && new_stat.command) {
    const auto* request = std::get_if<PlayerReviveCommand>(&revive.command->payload);
    KCD2S_CHECK(request && request->player == fixture.owner);
    KCD2S_CHECK(old_stat.command->sequence < revive.command->sequence);
    KCD2S_CHECK(revive.command->sequence < new_stat.command->sequence);
    KCD2S_CHECK(revive.command->scope == scope);
  }
  // Server::revive bumps FORCE_STATE. That combat fence must not discard its queued revival.
  KCD2S_CHECK(fixture.server->world().slot(fixture.owner)->stateEpoch != original_epoch);
  KCD2S_CHECK(fixture.client->take_player_engine_command(scope).status ==
              PlayerEngineCommandStatus::Empty);
  KCD2S_CHECK(fixture.client->take_stat_requests().empty());
  KCD2S_CHECK_EQ(fixture.client->take_revives(), std::size_t(0));
}

KCD2S_TEST(player_engine_commands, add_stat_keeps_its_place_among_set_stat_requests) {
  Fixture fixture;
  const auto scope = fixture.scope();
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {3, 1000000.0f}));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {3, -12.5f, true}));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {2, 40.0f}));
  // A change addStat refuses never leaves the server.
  KCD2S_CHECK(!fixture.server->request_stat(fixture.owner, {3, 2000000.0f, true}));
  fixture.pump();

  const StatRequest expected[] = {{3, 1000000.0f}, {3, -12.5f, true}, {2, 40.0f}};
  std::uint64_t previous = 0;
  for (const auto& wanted : expected) {
    const auto poll = fixture.client->take_player_engine_command(scope);
    KCD2S_CHECK(poll.status == PlayerEngineCommandStatus::Command);
    const auto* request = poll.command ? std::get_if<StatRequest>(&poll.command->payload) : nullptr;
    KCD2S_CHECK(request && *request == wanted);
    if (poll.command) {
      KCD2S_CHECK(poll.command->sequence > previous);
      previous = poll.command->sequence;
    }
  }
  KCD2S_CHECK(fixture.client->take_player_engine_command(scope).status ==
              PlayerEngineCommandStatus::Empty);
  // The queue takes a relative change below zero, never such a value.
  PlayerEngineCommandQueue queue;
  queue.reset_world(1);
  KCD2S_CHECK(queue.push_stat(1, {1, -5.0f, true}));
  KCD2S_CHECK(!queue.push_stat(1, {1, -5.0f}));
}

KCD2S_TEST(player_engine_commands, needs_reach_a_client_with_a_world) {
  Fixture fixture;
  KCD2S_CHECK(!fixture.client->take_needs().has_value());
  KCD2S_CHECK(fixture.server->send_needs(fixture.owner, false));
  fixture.pump();
  const auto needs = fixture.client->take_needs();
  KCD2S_CHECK(needs.has_value() && !*needs);
  KCD2S_CHECK(!fixture.client->take_needs().has_value());
  KCD2S_CHECK(!fixture.server->send_needs(fixture.owner + 100, true));
}

KCD2S_TEST(player_engine_commands, bound_stream_survives_knockout_and_revive_epoch_changes) {
  Fixture fixture;
  const auto scope = fixture.scope();
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  fixture.queue_stat_revive_stat(kcd2s_ctx);
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));

  check_stat(kcd2s_ctx, fixture.client->take_player_engine_command(scope), scope, 5);
  const auto revive = fixture.client->take_player_engine_command(scope);
  KCD2S_CHECK(revive.command &&
              std::holds_alternative<PlayerReviveCommand>(revive.command->payload));
  check_stat(kcd2s_ctx, fixture.client->take_player_engine_command(scope), scope, 80);
}

KCD2S_TEST(player_engine_commands, legacy_drains_consume_the_same_stream_only_once) {
  Fixture fixture;
  fixture.queue_stat_revive_stat(kcd2s_ctx);
  const auto legacy = fixture.client->take_stat_requests();
  KCD2S_CHECK_EQ(legacy.size(), std::size_t(2));
  KCD2S_CHECK(legacy.size() == 2 && legacy[0].value == 5 && legacy[1].value == 80);
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(fixture.scope()));

  const auto remaining = fixture.client->take_player_engine_command(fixture.scope());
  KCD2S_CHECK(remaining.command &&
              std::holds_alternative<PlayerReviveCommand>(remaining.command->payload));
  KCD2S_CHECK(fixture.client->take_player_engine_command(fixture.scope()).status ==
              PlayerEngineCommandStatus::Empty);
  KCD2S_CHECK(fixture.client->take_stat_requests().empty());
  KCD2S_CHECK_EQ(fixture.client->take_revives(), std::size_t(0));
}

KCD2S_TEST(player_engine_commands, new_world_discards_old_revive_and_adopts_new_staging) {
  Fixture fixture;
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(fixture.scope()));
  fixture.queue_stat_revive_stat(kcd2s_ctx);
  WorldConfig config;
  config.revision = 2;
  KCD2S_CHECK(fixture.server->configure_world(fixture.peer, config));
  fixture.pump();
  KCD2S_CHECK(fixture.client->take_stat_requests().empty());
  KCD2S_CHECK_EQ(fixture.client->take_revives(), std::size_t(0));

  KCD2S_CHECK(fixture.client->mark_world_ready(2));
  fixture.pump();
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 25}));
  fixture.pump();
  const auto scope = fixture.scope(2, 2);
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  check_stat(kcd2s_ctx, fixture.client->take_player_engine_command(scope), scope, 25);
  KCD2S_CHECK(fixture.client->take_player_engine_command(scope).status ==
              PlayerEngineCommandStatus::Empty);
}

KCD2S_TEST(player_engine_commands, same_poll_configuration_and_orders_wait_for_ready_body_bind) {
  replication::SchemaRegistry schema;
  const auto type = schema.define("Player").id();
  auto links = transport::make_loopback_pair();
  transport::Connection sender(links.first.get());
  ClientPeer client(schema, links.second.get());
  constexpr core::EntityId owner = 7;
  const PlayerEngineScope scope{1, owner, 1};
  sender.queue(0, encode_world_config({}));
  sender.queue(0, encode_set_owner(owner, type, 1));
  sender.queue(0, encode_set_stat({0, 5}));
  sender.queue(0, encode_revive(owner));
  sender.flush(0);
  client.tick(1.0 / 60);

  KCD2S_CHECK(!client.world_ready());
  KCD2S_CHECK(!client.current_player_engine_commands(scope));
  KCD2S_CHECK(client.take_stat_requests().empty());
  KCD2S_CHECK_EQ(client.take_revives(), std::size_t(0));
  KCD2S_CHECK(client.bind_player_engine_commands(scope));
  KCD2S_CHECK(!client.current_player_engine_commands(scope));
  KCD2S_CHECK(client.take_player_engine_command(scope).status ==
              PlayerEngineCommandStatus::ScopeMismatch);
  KCD2S_CHECK(client.mark_world_ready(1));
  KCD2S_CHECK(client.current_player_engine_commands(scope));
  check_stat(kcd2s_ctx, client.take_player_engine_command(scope), scope, 5);
  const auto revive = client.take_player_engine_command(scope);
  KCD2S_CHECK(revive.command &&
              std::holds_alternative<PlayerReviveCommand>(revive.command->payload));
}

KCD2S_TEST(player_engine_commands, body_replacement_drops_old_receipts_and_stale_poll_keeps_new) {
  Fixture fixture;
  const auto old_scope = fixture.scope();
  const auto replacement = fixture.scope(2);
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(old_scope));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 5}));
  fixture.pump();
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(replacement));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 70}));
  fixture.pump();

  KCD2S_CHECK(fixture.client->take_player_engine_command(old_scope).status ==
              PlayerEngineCommandStatus::ScopeMismatch);
  check_stat(kcd2s_ctx, fixture.client->take_player_engine_command(replacement), replacement, 70);
  KCD2S_CHECK(!fixture.client->bind_player_engine_commands(old_scope));
}

KCD2S_TEST(player_engine_commands, overflow_is_visible_and_cannot_be_cleared_by_same_scope_bind) {
  PlayerEngineCommandQueue queue;
  const PlayerEngineScope scope{1, 7, 1};
  queue.reset_world(1);
  KCD2S_CHECK(queue.bind(scope));
  for (std::size_t index = 0; index < PlayerEngineCommandQueue::kCapacity; ++index) {
    KCD2S_CHECK(index % 2 == 0 ? queue.push_stat(1, {0, 5}) : queue.push_revive(1, 7));
  }
  KCD2S_CHECK_EQ(queue.diagnostics().pending_commands, PlayerEngineCommandQueue::kCapacity);
  KCD2S_CHECK(!queue.push_stat(1, {0, 10}));
  const auto diagnostics = queue.diagnostics();
  KCD2S_CHECK(diagnostics.poisoned);
  KCD2S_CHECK_EQ(diagnostics.overflows, std::uint64_t(1));
  KCD2S_CHECK_EQ(diagnostics.pending_commands, std::size_t(0));
  const auto failed = queue.take(scope);
  KCD2S_CHECK(failed.status == PlayerEngineCommandStatus::Overflow);
  KCD2S_CHECK(!failed.command);
  KCD2S_CHECK_EQ(failed.overflows, std::uint64_t(1));
  KCD2S_CHECK_EQ(failed.discarded_commands, std::uint64_t(65));
  KCD2S_CHECK(queue.bind(scope));
  KCD2S_CHECK(!queue.push_revive(1, 7));
  KCD2S_CHECK(queue.take(scope).status == PlayerEngineCommandStatus::Overflow);

  const PlayerEngineScope replacement{1, 7, 2};
  KCD2S_CHECK(queue.bind(replacement));
  KCD2S_CHECK(queue.push_stat(1, {0, 30}));
  const auto fresh = queue.take(replacement);
  check_stat(kcd2s_ctx, fresh, replacement, 30);
  KCD2S_CHECK_EQ(fresh.overflows, std::uint64_t(1));
}

KCD2S_TEST(player_engine_commands,
           staged_overflow_remains_reported_after_first_bind_and_world_reset) {
  PlayerEngineCommandQueue queue;
  queue.reset_world(1);
  for (std::size_t index = 0; index < PlayerEngineCommandQueue::kCapacity; ++index) {
    KCD2S_CHECK(queue.push_stat(1, {0, 5}));
  }
  KCD2S_CHECK(!queue.push_revive(1, 7));
  KCD2S_CHECK(queue.bind({1, 7, 1}));
  KCD2S_CHECK(queue.take({1, 7, 1}).status == PlayerEngineCommandStatus::Overflow);

  queue.reset_world(2);
  KCD2S_CHECK(queue.push_revive(2, 7));
  KCD2S_CHECK(queue.bind({2, 7, 2}));
  const auto fresh = queue.take({2, 7, 2});
  KCD2S_CHECK(fresh.command && std::holds_alternative<PlayerReviveCommand>(fresh.command->payload));
  KCD2S_CHECK_EQ(fresh.overflows, std::uint64_t(1));
}

KCD2S_TEST(player_engine_commands, zero_health_has_no_portable_knockout_or_wake_policy) {
  PlayerEngineCommandQueue queue;
  const PlayerEngineScope scope{1, 7, 1};
  queue.reset_world(1);
  KCD2S_CHECK(queue.push_stat(1, {0, 0}));
  KCD2S_CHECK(queue.push_revive(1, 7));
  KCD2S_CHECK(queue.push_stat(1, {0, 50}));
  KCD2S_CHECK(queue.bind(scope));

  // Each pop leaves the rest pending. Only the native consumer decides whether a zero-health
  // attempt actually caused KO, or whether a revival finished waking the body.
  check_stat(kcd2s_ctx, queue.take(scope), scope, 0);
  const auto revive = queue.take(scope);
  KCD2S_CHECK(revive.command &&
              std::holds_alternative<PlayerReviveCommand>(revive.command->payload));
  check_stat(kcd2s_ctx, queue.take(scope), scope, 50);
}

KCD2S_TEST(player_engine_commands, first_bind_validates_staged_revive_owner_without_losing_stats) {
  PlayerEngineCommandQueue queue;
  const PlayerEngineScope scope{1, 7, 1};
  queue.reset_world(1);
  KCD2S_CHECK(queue.push_stat(1, {0, 15}));
  KCD2S_CHECK(queue.push_revive(1, 8));
  KCD2S_CHECK(queue.push_revive(1, 7));
  KCD2S_CHECK(queue.bind(scope));

  check_stat(kcd2s_ctx, queue.take(scope), scope, 15);
  const auto revive = queue.take(scope);
  const auto* request =
      revive.command ? std::get_if<PlayerReviveCommand>(&revive.command->payload) : nullptr;
  KCD2S_CHECK(request && request->player == 7);
  KCD2S_CHECK_EQ(revive.discarded_commands, std::uint64_t(1));
  KCD2S_CHECK(!queue.push_revive(1, 8));
}

KCD2S_TEST(player_engine_commands, invalidated_body_rejects_unbound_arrivals_and_old_token_reuse) {
  PlayerEngineCommandQueue queue;
  const PlayerEngineScope scope{1, 7, 1};
  queue.reset_world(1);
  KCD2S_CHECK(queue.bind(scope));
  KCD2S_CHECK(queue.push_revive(1, 7));
  queue.invalidate_body();
  KCD2S_CHECK(!queue.push_stat(1, {0, 5}));
  KCD2S_CHECK(!queue.bind(scope));
  const PlayerEngineScope replacement{1, 7, 2};
  KCD2S_CHECK(queue.bind(replacement));
  KCD2S_CHECK(queue.take(replacement).status == PlayerEngineCommandStatus::Empty);
}

KCD2S_TEST(player_engine_commands, session_end_invalidates_commands_even_after_reason_is_consumed) {
  Fixture fixture;
  const auto scope = fixture.scope();
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 5}));
  KCD2S_CHECK(fixture.server->send_session_end(fixture.owner, {SessionEndKind::Closed, "done"}));
  fixture.pump();
  KCD2S_CHECK(fixture.client->take_session_end().has_value());

  KCD2S_CHECK(fixture.client->take_player_engine_command(scope).status ==
              PlayerEngineCommandStatus::ScopeMismatch);
  KCD2S_CHECK(!fixture.client->bind_player_engine_commands(fixture.scope(2)));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 80}));
  fixture.pump();
  KCD2S_CHECK(fixture.client->take_stat_requests().empty());
}

KCD2S_TEST(player_engine_commands, session_end_revokes_a_popped_command_without_consuming_reason) {
  Fixture fixture;
  const auto scope = fixture.scope();
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 5}));
  fixture.pump();
  const auto popped = fixture.client->take_player_engine_command(scope);
  check_stat(kcd2s_ctx, popped, scope, 5);
  KCD2S_CHECK(fixture.client->current_player_engine_commands(scope));

  // The network thread can receive SESSION_END between the engine's pop and native setter.
  KCD2S_CHECK(fixture.server->send_session_end(fixture.owner, {SessionEndKind::Closed, "done"}));
  fixture.pump();
  KCD2S_CHECK(fixture.client->world_ready());
  KCD2S_CHECK(fixture.client->world_config()->revision == scope.revision);
  KCD2S_CHECK(fixture.client->owns(scope.owner));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(scope));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(scope));
  const auto reason = fixture.client->take_session_end();
  KCD2S_CHECK(reason && reason->reason == "done");
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(scope));
}

KCD2S_TEST(player_engine_commands, current_scope_observation_never_consumes_or_adopts_commands) {
  Fixture fixture;
  const auto original = fixture.scope();
  const auto replacement = fixture.scope(2);
  KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 5}));
  fixture.pump();
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(original));
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(original));
  const auto pending = fixture.client->player_engine_command_diagnostics().pending_commands;
  KCD2S_CHECK(fixture.client->current_player_engine_commands(original));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(replacement));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands({1, original.owner + 1, 1}));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands({2, original.owner, 1}));
  KCD2S_CHECK_EQ(fixture.client->player_engine_command_diagnostics().pending_commands, pending);
  check_stat(kcd2s_ctx, fixture.client->take_player_engine_command(original), original, 5);
  KCD2S_CHECK(fixture.client->current_player_engine_commands(original));

  KCD2S_CHECK(fixture.client->bind_player_engine_commands(replacement));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(original));
  KCD2S_CHECK(fixture.client->current_player_engine_commands(replacement));
  fixture.client->invalidate_player_engine_commands();
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(replacement));
}

KCD2S_TEST(player_engine_commands, poisoned_stream_revokes_current_scope_until_real_replacement) {
  Fixture fixture;
  const auto scope = fixture.scope();
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  KCD2S_CHECK(fixture.client->current_player_engine_commands(scope));
  for (std::size_t index = 0; index <= PlayerEngineCommandQueue::kCapacity; ++index) {
    KCD2S_CHECK(fixture.server->request_stat(fixture.owner, {0, 5}));
  }
  fixture.pump(16);
  const auto diagnostics = fixture.client->player_engine_command_diagnostics();
  KCD2S_CHECK(diagnostics.poisoned);
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(scope));
  KCD2S_CHECK_EQ(fixture.client->player_engine_command_diagnostics().overflows,
                 diagnostics.overflows);
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(scope));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(scope));
  const auto replacement = fixture.scope(2);
  KCD2S_CHECK(fixture.client->bind_player_engine_commands(replacement));
  KCD2S_CHECK(fixture.client->current_player_engine_commands(replacement));
  KCD2S_CHECK(!fixture.client->current_player_engine_commands(scope));
}
