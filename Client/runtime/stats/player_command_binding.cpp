#include "player_command_binding.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../knockout/body_state.hpp"
#include "../knockout/knockout.hpp"
#include "../knockout/knockout_tick.hpp"
#include "../riding/trample.hpp"
#include "player_commands.hpp"
#include "player_stats.hpp"

#include <limits>
#include <optional>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

PlayerCommandBinding player_command_binding;
std::uint64_t player_body_incarnation = 0;

void reset_player_command_binding() {
  std::lock_guard<std::mutex> lock(network_mutex);
  if (client && player_command_binding.scope.incarnation &&
      player_command_binding.session == network_client_generation) {
    const auto& configured = client->peer().world_config();
    // WORLD_CONFIG already discarded the old queue. Preserve arrivals staged for the new map.
    if (configured && configured->revision == player_command_binding.scope.revision) {
      client->peer().invalidate_player_engine_commands();
    }
  }
  player_command_binding = {};
}

bool bind_player_command_body(void* actor, std::uint32_t revision, core::EntityId owner) {
  auto soul = actor ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 1640) : nullptr;
  if (!soul || !revision || !owner) {
    return false;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (!client) {
    return false;
  }
  if (player_command_binding.actor == actor && player_command_binding.soul == soul &&
      player_command_binding.scope.revision == revision &&
      player_command_binding.scope.owner == owner &&
      player_command_binding.session == network_client_generation) {
    return !player_command_binding.failed;
  }
  if (player_body_incarnation == std::numeric_limits<std::uint64_t>::max()) {
    log("PLAYER COMMANDS body incarnation exhausted; native command stream disabled");
    return false;
  }
  const integration::PlayerEngineScope scope{revision, owner, ++player_body_incarnation};
  if (!client->peer().bind_player_engine_commands(scope)) {
    return false;
  }
  player_command_binding = {};
  player_command_binding.scope = scope;
  player_command_binding.actor = actor;
  player_command_binding.soul = soul;
  player_command_binding.session = network_client_generation;
  log("PLAYER COMMANDS bound revision=%u owner=%u body=%llu session=%llu", revision, owner,
      scope.incarnation, network_client_generation);
  return true;
}

struct PlayerCommandContext {
  integration::PlayerEngineScope scope;
  void* actor = nullptr;
  void* soul = nullptr;
  std::uint64_t session = 0;
};

bool current_player_command_body(void* opaque, const integration::PlayerEngineScope& scope) {
  const auto& context = *static_cast<const PlayerCommandContext*>(opaque);
  if (!(context.scope == scope) || !(player_command_binding.scope == scope) ||
      player_command_binding.failed || local_combat_actor.load() != context.actor ||
      !context.actor ||
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(context.actor) + 1640) != context.soul) {
    return false;
  }
  std::lock_guard<std::mutex> lock(network_mutex);
  if (!client || context.session != network_client_generation) {
    return false;
  }
  return client->own_avatar() == scope.owner &&
         client->peer().current_player_engine_commands(scope);
}

void run_player_commands() {
  if (!player_command_binding.scope.incarnation || player_command_binding.failed) {
    return;
  }
  integration::PlayerEngineCommandDiagnostics diagnostics;
  {
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!client || player_command_binding.session != network_client_generation) {
      return;
    }
    diagnostics = client->peer().player_engine_command_diagnostics();
  }
  if (diagnostics.overflows != player_command_binding.observed_overflows) {
    player_command_binding.observed_overflows = diagnostics.overflows;
    log("PLAYER COMMANDS overflow=%llu discarded=%llu revision=%u owner=%u body=%llu",
        diagnostics.overflows, diagnostics.discarded_commands,
        player_command_binding.scope.revision, player_command_binding.scope.owner,
        player_command_binding.scope.incarnation);
  }
  if (diagnostics.poisoned) {
    player_command_binding.failed = true;
    log("PLAYER COMMANDS poisoned; current body command stream stopped");
    return;
  }
  PlayerCommandContext context{player_command_binding.scope, player_command_binding.actor,
                               player_command_binding.soul, player_command_binding.session};
  kcds::runtime::stats::PlayerCommandCallbacks callbacks;
  callbacks.context = &context;
  callbacks.pop = [](void* opaque, const integration::PlayerEngineScope& scope) {
    const auto& context = *static_cast<const PlayerCommandContext*>(opaque);
    std::lock_guard<std::mutex> lock(network_mutex);
    if (!client || context.session != network_client_generation) {
      return integration::PlayerEngineCommandPoll{
          integration::PlayerEngineCommandStatus::ScopeMismatch, std::nullopt, 0, 0};
    }
    return client->peer().take_player_engine_command(scope);
  };
  callbacks.is_current = current_player_command_body;
  callbacks.observe = [](void* opaque) {
    const auto& context = *static_cast<const PlayerCommandContext*>(opaque);
    auto soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(context.actor) + 1640);
    return kcds::runtime::knockout::observe_body(soul, context.soul, native_soul_is_unconscious);
  };
  callbacks.apply_stat = [](void* opaque, const integration::StatRequest& request) {
    const auto& context = *static_cast<const PlayerCommandContext*>(opaque);
    kcds::runtime::stats::apply(context.soul, {request}, false, queue_scripted_health_reduction);
    // setHealth(0, { fall: true }): the body drops at once with the game's sleeping ragdoll
    // profile, as a trampled body does (riding/trample.hpp), instead of standing about half a
    // second before the game's ragdoll topples it.
    if (request.fall && context.actor) {
      player_command_binding.fell_asleep =
          set_physics_profile(context.actor, PhysicsProfile::Sleep);
      log("PLAYER COMMANDS knock-out falls at once");
    }
  };
  callbacks.revive = [](void* opaque) {
    const auto& context = *static_cast<const PlayerCommandContext*>(opaque);
    // A body that fell at once stays the sleeping ragdoll the fall made it: the end of its
    // unconsciousness alone left it lying for good on every screen. It is made
    // alive again first, as a trampled body is when its fall ends (riding/trample.cpp), so the
    // game's own getting up plays.
    if (player_command_binding.fell_asleep && context.actor) {
      const bool alive = set_physics_profile(context.actor, PhysicsProfile::Alive);
      player_command_binding.fell_asleep = false;
      log("PLAYER COMMANDS revive of a body that fell at once: alive again=%u", unsigned(alive));
    }
    discard_held_hits("ordered local revive");
    // Completed earlier setters belong before this revive. Cancelled pending contacts must not
    // hold those reports behind an old-life ruling barrier.
    send_player_damage();
    discard_player_damage("ordered local revive");
    return kcds::runtime::knockout::revive();
  };
  callbacks.knock_out = [](void*, float health) {
    return kcds::runtime::knockout::knock_out(health);
  };
  callbacks.after_mutation = [](void* opaque) {
    tick_knockout(static_cast<const PlayerCommandContext*>(opaque)->actor);
  };
  const bool was_waiting = player_command_binding.runner.waiting_for_native_wake();
  const auto result = player_command_binding.runner.run_tick(context.scope, callbacks);
  if (result.popped || result.status == kcds::runtime::stats::PlayerCommandRunStatus::Failed ||
      was_waiting != player_command_binding.runner.waiting_for_native_wake()) {
    log("PLAYER COMMANDS status=%u popped=%zu applied=%zu dropped=%zu revives=%zu "
        "knock_outs=%zu dropped_knock_outs=%zu failure=%u last=%llu body=%llu",
        unsigned(result.status), result.popped, result.applied_stats, result.dropped_stats,
        result.revives, result.knock_outs, result.dropped_knock_outs, unsigned(result.failure),
        result.last_sequence, context.scope.incarnation);
  }
  if (result.status == kcds::runtime::stats::PlayerCommandRunStatus::Failed) {
    player_command_binding.failed = true;
    log("PLAYER COMMANDS native stream failed; waiting for actual body/session replacement");
  }
}

}  // namespace kcds::game_client
