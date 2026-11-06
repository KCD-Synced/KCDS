// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "defence.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "clinch_relay.hpp"
#include "combat_identity.hpp"
#include "defence_timing.hpp"

#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// Perfect block. A block press reaches the attacker's client a round trip late, too late
// for that client's perfect-block window, so there the defender's replica only blocked and the
// attacker's game gave its swing a plain deflection while the defender's own game played the
// perfect block, stepping in up to 1.5 m. The owner's perfect block is relayed and its
// replicas start the same native one. (the block trigger's perfect-block start)
// calls(movement, &action, 1, variant, 0, request id): it builds the
// PERFECT_BLOCK action (type 7) against the movement's lock target (core+3320) and pushes it.
// The block trigger is one of several callers of (kinds 1 perfect block, 4, 5, 6); the
// start itself is hooked and a kind-1 start that made an action on the local fighter is relayed.
StartDefence start_defence = nullptr;
StartDefence original_start_defence = nullptr;
const unsigned* perfect_block_request = nullptr;
// A combat model's int property set as the game sets it (the value at + 8, then
// its listeners at + 24 are told). The guard zone is such a property at core + 240 (its value at
// core + 248, the native getter reads + 8).
SetCombatIntProperty set_combat_int_property = nullptr;
// The guard zone (kCoreGuard) and the pair-row mode (kCoreMode): core/client_internal.hpp.
// The guard and the core + 96 value the owner of each replica had at its last relayed
// SYNC_PERFECT_BLOCK start (DefenceGuard, DefenceMode), taken by the replica's start that follows.
std::unordered_map<core::EntityId, unsigned> relayed_defence_guards;
std::unordered_map<core::EntityId, unsigned> relayed_defence_modes;
// Each replica's owner's values of those (CombatModel), which the replica keeps: a pair it plays
// here swaps its own stance as the owner's did on its screen, and its guard follows relayed mouse
// deltas. ~0 until the owner sent one.
std::unordered_map<core::EntityId, std::array<unsigned, 3>> owner_models;

// Where a fighter and its lock target stand for a pair's placement (places the
// rows' animations by both fighters): their distance and how far each faces off the line to
// the other, in degrees. The entity's world matrix is at entity + 88 (row major 3x4).
struct PairPose {
  float distance = -1.0f;
  float own_off = 0.0f;
  float opponent_off = 0.0f;
};

PairPose pair_pose(void* movement) {
  PairPose pose;
  auto movement_bytes = static_cast<std::uint8_t*>(movement);
  auto core = movement_bytes ? *reinterpret_cast<std::uint8_t**>(movement_bytes + 528) : nullptr;
  auto opponent = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
  if (!opponent) {
    return pose;
  }
  const auto matrix = [](std::uint8_t* fighter_movement) -> const float* {
    auto actor = *reinterpret_cast<std::uint8_t**>(fighter_movement + 504);
    auto entity = actor ? *reinterpret_cast<std::uint8_t**>(actor + 56) : nullptr;
    return entity ? reinterpret_cast<const float*>(entity + 88) : nullptr;
  };
  const float* own = matrix(movement_bytes);
  const float* other = matrix(opponent);
  if (!own || !other) {
    return pose;
  }
  const float dx = other[3] - own[3];
  const float dy = other[7] - own[7];
  pose.distance = std::sqrt(dx * dx + dy * dy);
  const float to_other = std::atan2(dy, dx);
  const auto off = [](const float* m, float direction) {
    // The entity's forward axis is its matrix's second column (x = m[1], y = m[5]).
    float delta = std::atan2(m[5], m[1]) - direction;
    while (delta > 3.14159265f) {
      delta -= 6.2831853f;
    }
    while (delta < -3.14159265f) {
      delta += 6.2831853f;
    }
    return std::fabs(delta) * 57.29578f;
  };
  pose.own_off = off(own, to_other);
  pose.opponent_off = off(other, to_other + 3.14159265f);
  return pose;
}

// What the pair-row query reads of the attacker (diagnostics): its combat state
// (core + 56; the Transition bit 1024 selects other rows) and the value of its movement + 944
// component's slot 4.
struct PairAttacker {
  unsigned state = ~0u;
  unsigned component = ~0u;
};

// The defender's and the attacker's core int properties the pair-row query copies
// (the defender's core + 96, + 192 and + 240, the attacker's core + 192), as
// "defender96/defender192/defender240/attacker192".
std::string pair_query_properties(void* movement) {
  const auto value = [](const std::uint8_t* core, std::size_t property) {
    return core ? *reinterpret_cast<const unsigned*>(core + property + 8) : ~0u;
  };
  auto movement_bytes = static_cast<std::uint8_t*>(movement);
  auto core = movement_bytes ? *reinterpret_cast<std::uint8_t**>(movement_bytes + 528) : nullptr;
  auto opponent = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
  auto opponent_core = opponent ? *reinterpret_cast<std::uint8_t**>(opponent + 528) : nullptr;
  char text[64];
  sprintf_s(text, "%u/%u/%u/%u", value(core, 96), value(core, 192), value(core, 240),
            value(opponent_core, 192));
  return text;
}

PairAttacker pair_attacker(void* movement) {
  PairAttacker attacker;
  auto movement_bytes = static_cast<std::uint8_t*>(movement);
  auto core = movement_bytes ? *reinterpret_cast<std::uint8_t**>(movement_bytes + 528) : nullptr;
  auto opponent = core ? *reinterpret_cast<std::uint8_t**>(core + 3320) : nullptr;
  if (!opponent) {
    return attacker;
  }
  auto opponent_core = *reinterpret_cast<std::uint8_t**>(opponent + 528);
  if (opponent_core) {
    attacker.state = *reinterpret_cast<unsigned*>(opponent_core + 56);
  }
  auto component = *reinterpret_cast<std::uint8_t**>(opponent + 944);
  if (component) {
    attacker.component = slot<unsigned(__fastcall*)(void*)>(component + 16, 4)(component + 16);
  }
  return attacker;
}

// The attack direction of a fighter's lock target (its core + 336 property, the value at + 344;
// reads it through the property's getter), ~0 when there is none.
unsigned opponent_attack_zone(const std::uint8_t* core) {
  if (!core) {
    return ~0u;
  }
  auto opponent = *reinterpret_cast<std::uint8_t* const*>(core + 3320);
  auto opponent_core = opponent ? *reinterpret_cast<std::uint8_t**>(opponent + 528) : nullptr;
  return opponent_core ? *reinterpret_cast<unsigned*>(opponent_core + 344) : ~0u;
}
// The local game made a SYNC_PERFECT_BLOCK during the input being handled (engine thread). An
// attack press it turned into the master strike is not relayed as an attack (hooked_combat_input):
// its replicas start the relayed master strike instead of a swing of their own (the replica
// swung first and its master strike came 280 ms late on the struck fighter's screen).
bool input_made_sync_perfect_block = false;
// That press was withheld: its release is too.
bool attack_press_withheld = false;
void relay_combat_action(const integration::CombatActionData& data);
void begin_local_combat_contact(void* action);
core::EntityId remote_combat_id(void* movement);
// Lets the game animate a player's replica itself for a moment (its block, parry or pair).
void open_reaction_window(core::EntityId player);

void** __fastcall hooked_start_defence(void* movement, void** action, char kind, unsigned variant,
                                       char flag, unsigned request) {
  auto local_actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto local_movement = local_actor ? *reinterpret_cast<void**>(local_actor + 632) : nullptr;
  // A replica's perfect block is its owner's, relayed (PerfectBlock; call_replica_defence_start
  // calls the game's start directly). One the replica's game would make of the owner's replayed
  // block press is left out (a replica parried where its owner's game made no parry
  // of the press, during its recovery, and riposted out of that parry).
  if (kind == kPerfectBlockKind && movement && movement != local_movement) {
    if (const auto owner = remote_combat_id(movement)) {
      log("COMBAT replica perfect block left to its owner's game source=%u", owner);
      if (action) {
        *action = nullptr;
      }
      return action;
    }
  }
  // What the pair-row query reads, before the start changes it (diagnostics).
  const std::string query_before =
      movement && movement == local_movement && kind != kPerfectBlockKind
          ? pair_query_properties(movement)
          : std::string();
  auto result = original_start_defence(movement, action, kind, variant, flag, request);
  // Every defence start of the local fighter, made or refused (block, parry, master strike).
  if (movement && movement == local_movement) {
    auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
    const PairPose pose = pair_pose(movement);
    const PairAttacker attacker = pair_attacker(movement);
    log("COMBAT DEFENCE start kind=%d variant=%u flag=%u request=%u made=%u guard=%u attack=%u "
        "distance=%.2f off=%.0f/%.0f attacker_state=%u attacker_component=%u query=%s",
        int(kind), variant, unsigned(flag), request, unsigned(action && *action),
        core ? *reinterpret_cast<unsigned*>(core + kCoreGuard + 8) : 0u, opponent_attack_zone(core),
        pose.distance, pose.own_off, pose.opponent_off, attacker.state, attacker.component,
        query_before.c_str());
    if (action && *action &&
        (kind == kPerfectBlockKind || kind == kMasterStrikeKind || kind == kPairPerfectBlockKind)) {
      // A native defence can start its own riposte without entering the ordinary attack factory.
      begin_local_combat_contact(*action);
    }
  }
  if (kind == kPerfectBlockKind && movement && movement == local_movement && action && *action) {
    relay_combat_action({integration::CombatAction::PerfectBlock, 1, float(variant)});
    log("COMBAT PERFECT BLOCK queue variant=%u flag=%u request=%u", variant, unsigned(flag),
        request);
  }
  const bool sync_perfect_block = kind == kMasterStrikeKind || kind == kPairPerfectBlockKind;
  if (sync_perfect_block && movement && movement == local_movement && action && *action) {
    auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
    // The guard this start was made from goes first: the replicas need it for the same pair rows.
    const unsigned guard = core ? *reinterpret_cast<unsigned*>(core + kCoreGuard + 8) : ~0u;
    if (guard <= kMaxGuardZone) {
      relay_combat_action({integration::CombatAction::DefenceGuard, 1, float(guard)});
    }
    const unsigned defence_mode = core ? *reinterpret_cast<unsigned*>(core + kCoreMode + 8) : ~0u;
    if (defence_mode <= kMaxDefenceMode) {
      relay_combat_action({integration::CombatAction::DefenceMode, 1, float(defence_mode)});
    }
    const unsigned relayed = request < kNoMasterStrikeRequest ? request : kNoMasterStrikeRequest;
    const std::uint8_t mode = kind == kMasterStrikeKind ? 1 : 2;
    relay_combat_action({integration::CombatAction::SyncPerfectBlock, mode, float(relayed)});
    log("COMBAT SYNC PERFECT BLOCK queue kind=%d variant=%u flag=%u request=%u guard=%u", int(kind),
        variant, unsigned(flag), request, guard);
    input_made_sync_perfect_block = true;
    // The pair moves both bodies at once: the opponent's replica plays its half here now, as in a
    // game alone, instead of waiting a round trip for its owner's clips of the same pair
    // (the two halves visibly apart).
    if (core) {
      open_reaction_window(remote_combat_id(*reinterpret_cast<void**>(core + 3320)));
    }
  }
  return result;
}

unsigned native_action_type(void* movement);
// The owner's perfect block on its replica: the same native start against the replica's lock
// target. The game starts a perfect block from a block, and the replica's block (the owner's
// replayed press) can land a few frames after the relayed perfect block (60 ms), so the
// start is retried every frame for a moment. Engine thread (PendingPerfectBlock, defence.hpp).
std::unordered_map<core::EntityId, PendingPerfectBlock> pending_perfect_blocks;
constexpr ULONGLONG kPerfectBlockWaitMs = 300;
// The entity of the replica whose attack is running here, other than `player`'s; nullptr for
// none (defined with the replicas).
void* running_attacker_entity(core::EntityId player);

// The perfect-block window lookup (C_CombatMultiTriggerPerfectBlock vtable slot
// 14): the defender's list of the windows its attackers' swings opened (each attacker's game adds
// its own, on the attacker's clock). On the attacker's client the owner's relayed
// perfect block arrives a round trip after that window closed, so the list is empty there; the
// owner's game already found the window open. only checks the looked-up window for a
// perfect block (kind 1), so during a relayed start on a replica the lookup answers with a
// stand-in. The local player's own defence is never widened: only the owner's decision is shown.
WindowLookup original_window_lookup = nullptr;
thread_local bool relayed_perfect_block = false;
std::uint8_t relayed_window_stand_in = 0;

void* __fastcall hooked_window_lookup(void* trigger, int attacker) {
  void* window = original_window_lookup(trigger, attacker);
  if (!window && relayed_perfect_block) {
    return &relayed_window_stand_in;
  }
  return window;
}

// A replica takes its owner's value of a combat int property (core + property) through the game's
// setter. Only a property of the local fighter's class is written.
void take_owner_property(void* movement, std::size_t property, unsigned value) {
  auto local_actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto local_movement =
      local_actor ? *reinterpret_cast<std::uint8_t**>(local_actor + 632) : nullptr;
  auto local_core =
      local_movement ? *reinterpret_cast<std::uint8_t**>(local_movement + 528) : nullptr;
  auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
  if (!set_combat_int_property || !core || !local_core ||
      *reinterpret_cast<void**>(core + property) !=
          *reinterpret_cast<void**>(local_core + property)) {
    return;
  }
  set_combat_int_property(core + property, int(value));
}

// A pair's rows need the defender's guard opposite the incoming attack
// (kind 4/5 through): the replica takes its owner's guard from that start first.
void take_owner_guard(void* movement, unsigned guard) {
  if (guard <= kMaxGuardZone) {
    take_owner_property(movement, kCoreGuard, guard);
  }
}

// The local fighter's combat-model properties go to the other clients whenever one changes
// so their replica of it keeps its owner's.
void relay_combat_model(void* actor) {
  static void* relayed_actor = nullptr;
  static std::array<unsigned, 3> relayed = {~0u, ~0u, ~0u};
  auto movement =
      actor ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632) : nullptr;
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  if (!core) {
    return;
  }
  if (actor != relayed_actor) {
    relayed_actor = actor;
    relayed = {~0u, ~0u, ~0u};
  }
  for (std::size_t index = 0; index < kCoreModelProperties.size(); ++index) {
    const unsigned value = *reinterpret_cast<unsigned*>(core + kCoreModelProperties[index] + 8);
    if (value == relayed[index] || value > kMaxDefenceMode) {
      continue;
    }
    relayed[index] = value;
    relay_combat_action(
        {integration::CombatAction::CombatModel, std::uint8_t(index + 1), float(value)});
  }
}

void call_replica_defence_start(void* movement, void** action, char kind, unsigned variant,
                                unsigned request, unsigned guard, unsigned mode) {
  const bool previous_relay = relayed_perfect_block;
  auto previous_actor = replicated_attack_actor;
  auto previous_opponent = replicated_opponent_actor;
  if (kind != kPerfectBlockKind) {
    take_owner_guard(movement, guard);
    if (mode <= kMaxDefenceMode) {
      take_owner_property(movement, kCoreMode, mode);
    }
    // On a watcher the attacker is a replica too: the pair's placement reads its core + 96 as well
    // which the game set late there.
    auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
    auto opponent = core ? *reinterpret_cast<void**>(core + 3320) : nullptr;
    if (auto model = owner_models.find(remote_combat_id(opponent)); model != owner_models.end()) {
      const unsigned opponent_mode = model->second[2];
      if (opponent_mode <= kMaxDefenceMode) {
        take_owner_property(opponent, kCoreMode, opponent_mode);
      }
    }
  }
  __try {
    relayed_perfect_block = original_window_lookup != nullptr;
    replicated_attack_actor = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(movement) + 504);
    replicated_opponent_actor = local_combat_actor.load();
    original_start_defence(movement, action, kind, variant, 0, request);
  } __finally {
    replicated_attack_actor = previous_actor;
    replicated_opponent_actor = previous_opponent;
    relayed_perfect_block = previous_relay;
  }
}

bool try_replica_perfect_block(core::EntityId player, const PendingPerfectBlock& pending) {
  const unsigned expected =
      pending.kind == kPerfectBlockKind ? kNativePerfectBlockAction : kNativeMasterStrikeAction;
  if (native_action_type(pending.movement) == expected) {
    log("COMBAT PERFECT BLOCK replica player=%u kind=%d already started", player,
        int(pending.kind));
    return true;
  }
  // A watcher's replica has no lock target (the owner's selection names only the client it
  // targets): it blocks the replica whose attack is running here.
  auto movement = static_cast<std::uint8_t*>(pending.movement);
  auto core = *reinterpret_cast<std::uint8_t**>(movement + 528);
  if (core && !*reinterpret_cast<void**>(core + 3320)) {
    if (void* attacker = running_attacker_entity(player)) {
      original_set_combat_target(movement, attacker);
    }
  }
  void* action = nullptr;
  // records both ActorHasChannel results in the native defence
  // query. Ordinary perfect block reaches it through ->;
  // paired defence reaches it through ->. Both must select
  // with the owner's input role. This scope changes neither actual channels nor
  // the local fighter's eligibility outside the relayed original start.
  const std::string query_before = pair_query_properties(pending.movement);
  call_replica_defence_start(pending.movement, &action, pending.kind, pending.variant,
                             pending.request, pending.guard, pending.mode);
  if (!action) {
    return false;
  }
  const PairPose pose = pair_pose(pending.movement);
  const PairAttacker attacker = pair_attacker(pending.movement);
  log("COMBAT PERFECT BLOCK replica player=%u kind=%d variant=%u started action=%u guard=%u "
      "attack=%u distance=%.2f off=%.0f/%.0f attacker_state=%u attacker_component=%u query=%s",
      player, int(pending.kind), pending.variant, native_action_type(pending.movement),
      core ? *reinterpret_cast<unsigned*>(core + kCoreGuard + 8) : 0u, opponent_attack_zone(core),
      pose.distance, pose.own_off, pose.opponent_off, attacker.state, attacker.component,
      query_before.c_str());
  // A pair (SYNC_PERFECT_BLOCK) moves both bodies at once: the replica plays its half here now,
  // with the local fighter's, its owner's clips of the same pair then not again.
  if (pending.kind != kPerfectBlockKind) {
    open_reaction_window(player);
  }
  // The start hands back a reference (releases it through vtable slot 2).
  (*reinterpret_cast<void(__fastcall***)(void*)>(action))[2](action);
  return true;
}

void start_replica_perfect_block(core::EntityId player, void* movement, unsigned variant, char kind,
                                 unsigned request) {
  if (!original_start_defence || !perfect_block_request || !movement) {
    return;
  }
  PendingPerfectBlock pending{movement, variant, GetTickCount64() + kPerfectBlockWaitMs, kind,
                              request};
  if (kind != kPerfectBlockKind) {
    if (auto guard = relayed_defence_guards.find(player); guard != relayed_defence_guards.end()) {
      pending.guard = guard->second;
      relayed_defence_guards.erase(guard);
    }
    if (auto mode = relayed_defence_modes.find(player); mode != relayed_defence_modes.end()) {
      pending.mode = mode->second;
      relayed_defence_modes.erase(mode);
    }
  }
  if (!try_replica_perfect_block(player, pending)) {
    if (kind != kPerfectBlockKind) {
      const PairPose pose = pair_pose(movement);
      const PairAttacker attacker = pair_attacker(movement);
      log("COMBAT PERFECT BLOCK replica player=%u kind=%d first start refused distance=%.2f "
          "off=%.0f/%.0f attacker_state=%u attacker_component=%u opponent_action=%u query=%s",
          player, int(kind), pose.distance, pose.own_off, pose.opponent_off, attacker.state,
          attacker.component,
          native_action_type(local_combat_actor.load()
                                 ? *reinterpret_cast<void**>(
                                       static_cast<std::uint8_t*>(local_combat_actor.load()) + 632)
                                 : nullptr),
          pair_query_properties(movement).c_str());
    }
    pending_perfect_blocks[player] = pending;
  }
}

void tick_pending_perfect_blocks() {
  const ULONGLONG now = GetTickCount64();
  for (auto entry = pending_perfect_blocks.begin(); entry != pending_perfect_blocks.end();) {
    if (try_replica_perfect_block(entry->first, entry->second)) {
      entry = pending_perfect_blocks.erase(entry);
    } else if (now >= entry->second.until) {
      // A master strike's pair rows need the defender's guard opposite the attack's direction
      auto core = *reinterpret_cast<std::uint8_t**>(
          static_cast<std::uint8_t*>(entry->second.movement) + 528);
      log("COMBAT PERFECT BLOCK replica player=%u not started (action=%u) kind=%d guard=%u "
          "attack=%u",
          entry->first, native_action_type(entry->second.movement), int(entry->second.kind),
          core ? *reinterpret_cast<unsigned*>(core + kCoreGuard + 8) : 0u,
          opponent_attack_zone(core));
      entry = pending_perfect_blocks.erase(entry);
    } else {
      ++entry;
    }
  }
}
// Observe the original quick-event predicate; never widen its timing,
// force its return value, or synthesize a successful defense.
PerfectBlockWindow original_perfect_block_window;
bool __fastcall hooked_perfect_block_window(void* trigger) {
  const bool allowed = original_perfect_block_window(trigger);
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(trigger) + 8);
  auto actor = movement ? *reinterpret_cast<void**>(movement + 504) : nullptr;
  const bool local = actor && actor == local_combat_actor.load();
  core::EntityId id = 0;
  if (!local && actor) {
    auto soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + 1640);
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    if (auto it = remote_combat_souls.find(soul); it != remote_combat_souls.end())
      id = it->second;
  }
  if (local || id) {
    static thread_local std::unordered_map<void*, bool> last;
    if (!last.count(trigger) || last[trigger] != allowed) {
      last[trigger] = allowed;
      log("COMBAT native trigger predicate local=%u replica=%u allowed=%u action=%u",
          unsigned(local), id, unsigned(allowed), native_action_type(movement));
    }
  }
  return allowed;
}

}  // namespace kcds::game_client
