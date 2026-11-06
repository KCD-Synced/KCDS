#include "dodge_relay.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../replicas/remote.hpp"
#include "dodges.hpp"

#include <cstring>
#include <optional>

namespace kcds::game_client {

namespace integration = kcd2s::integration;

// Dodges (combat/dodges.hpp). The owner's game makes a dodge from its jump key and the
// direction held; its client relays the direction (Dodge), and every other client's game makes the
// same native dodge (action 8, combat state 512) on its replica through the replica's own dodge
// request. The game then plays the dodge's own clip there, and its checks see a dodging fighter
// there as on its owner's screen (no defence window is valid for a defender who strikes or dodges,
// combat states 8 and 512). Before, the replica only played its owner's clip and
// stayed in its guard (owner action 8 and clip 4537, the replica's REMOTE
// ANIMATION 4537 with no native action). The jump key is not replayed, and a dodge a replica's game
// would make by itself is refused, so a replica dodges only where its owner's game did.
CreateDodge original_create_dodge = nullptr;
RequestDodge request_dodge = nullptr;
// The fighter's combat controller (movement + 1104): its movement at + 24, its input filter at + 56
// (which the request's filter reads unchecked,->) and the dodge request's
// two input vectors (two floats each) at + 104 and + 112.
constexpr std::size_t kMovementCombatController = 1104;
constexpr std::size_t kControllerMovement = 24;
constexpr std::size_t kControllerInputFilter = 56;
constexpr std::size_t kControllerKeys = 104;
constexpr std::size_t kControllerSecondInput = 112;
// The dodge windows its opponents' attacks opened on a fighter now (core + 2056, an int): the
// dodge's start makes it a slot dodge while one is open (sets action + 240 from it).
constexpr std::size_t kCoreDodgeWindows = 2056;
// The filter the dodge request hands to the creation is a std::function taken by value: its target
// at + 56, destroyed by the creation on every path (the target's vtable slot 4, told
// whether the target lies outside the function's own storage).
constexpr std::size_t kFunctionTarget = 56;
constexpr std::size_t kFunctionDestroySlot = 4;
// The replica movement whose owner's dodge its game is making right now, and whether it made it
// (engine thread).
void* relayed_dodge_movement = nullptr;
bool relayed_dodge_made = false;
// When the local fighter's game last made a dodge in its slot (engine thread).
std::uint64_t last_slot_dodge_at = 0;

std::uint8_t* combat_controller_of(void* movement) {
  if (!movement) {
    return nullptr;
  }
  return *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) +
                                           kMovementCombatController);
}

kcds::runtime::combat::DodgeVector controller_vector(const std::uint8_t* controller,
                                                     std::size_t at) {
  kcds::runtime::combat::DodgeVector vector;
  std::memcpy(&vector.x, controller + at, sizeof(float));
  std::memcpy(&vector.y, controller + at + sizeof(float), sizeof(float));
  return vector;
}

void set_controller_vector(std::uint8_t* controller, std::size_t at,
                           const kcds::runtime::combat::DodgeVector& vector) {
  std::memcpy(controller + at, &vector.x, sizeof(float));
  std::memcpy(controller + at + sizeof(float), &vector.y, sizeof(float));
}

int open_dodge_windows(void* movement) {
  auto core = movement
                  ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528)
                  : nullptr;
  return core ? *reinterpret_cast<int*>(core + kCoreDodgeWindows) : -1;
}

// A creation left out as the game's own refused creation ends: the filter it was handed goes.
void destroy_dodge_filter(void* filter) {
  auto storage = static_cast<std::uint8_t*>(filter);
  auto target = storage ? *reinterpret_cast<std::uint8_t**>(storage + kFunctionTarget) : nullptr;
  if (!target) {
    return;
  }
  using Destroy = void(__fastcall*)(void* target, bool outside);
  slot<Destroy>(target, kFunctionDestroySlot)(target, target != storage);
  *reinterpret_cast<void**>(storage + kFunctionTarget) = nullptr;
}

// The local fighter's game made a dodge, or refused one: the direction its request dodged in goes
// to every other client.
void relay_local_dodge(void* movement, bool made) {
  auto controller = combat_controller_of(movement);
  std::optional<float> degrees;
  if (controller) {
    const auto keys = controller_vector(controller, kControllerKeys);
    const auto second = controller_vector(controller, kControllerSecondInput);
    degrees = kcds::runtime::combat::dodge_degrees(keys, second);
  }
  const int windows = open_dodge_windows(movement);
  log("COMBAT DODGE local made=%u degrees=%.0f windows=%d", unsigned(made),
      degrees ? *degrees : 0.0f, windows);
  if (!made) {
    return;
  }
  if (windows > 0) {
    last_slot_dodge_at = GetTickCount64();
  }
  if (!degrees) {
    log("COMBAT DODGE local made without an input direction: not relayed");
    return;
  }
  relay_combat_action({integration::CombatAction::Dodge, 1, *degrees});
}

// (movement, out action, the selected dodge row, filter): creates the dodge and
// pushes it to the fighter's action driver; *out is the dodge when it was made.
void** __fastcall hooked_create_dodge(void* movement, void** action, const void* row,
                                      void* filter) {
  auto local_actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto local_movement = local_actor ? *reinterpret_cast<void**>(local_actor + 632) : nullptr;
  const bool local = movement && movement == local_movement;
  if (!local && movement && movement != relayed_dodge_movement) {
    if (const auto owner = remote_combat_id(movement)) {
      destroy_dodge_filter(filter);
      if (action) {
        *action = nullptr;
      }
      log("COMBAT DODGE replica dodge left to its owner's game source=%u", owner);
      return action;
    }
  }
  auto result = original_create_dodge(movement, action, row, filter);
  const bool made = action && *action;
  if (movement && movement == relayed_dodge_movement) {
    relayed_dodge_made = made;
  }
  if (local) {
    relay_local_dodge(movement, made);
  }
  return result;
}

// The replica's own dodge request, with its owner's direction as the request's input for this one
// call and the replica counted as a player against a replica, as its owner's game had it (the
// thread's scopes are put back however the call ends).
void call_relayed_dodge_request(std::uint8_t* controller, void* movement, void* replica_actor) {
  auto previous_actor = replicated_attack_actor;
  auto previous_opponent = replicated_opponent_actor;
  __try {
    relayed_dodge_movement = movement;
    relayed_dodge_made = false;
    replicated_attack_actor = replica_actor;
    replicated_opponent_actor = local_combat_actor.load();
    request_dodge(controller);
  } __finally {
    replicated_attack_actor = previous_actor;
    replicated_opponent_actor = previous_opponent;
    relayed_dodge_movement = nullptr;
  }
}

// The owner's dodge on its replica (engine thread), started once when the owner's Dodge action
// arrives: the replica's game makes the dodge or refuses it, and a refusal is logged and not tried
// again.
void start_replica_dodge(core::EntityId player, float degrees) {
  const auto replica = remotes.find(player);
  if (replica == remotes.end() || !replica->second.actor) {
    log("COMBAT DODGE replica player=%u gone", player);
    return;
  }
  auto movement =
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(replica->second.actor) + 632);
  auto controller = combat_controller_of(movement);
  auto local_actor = static_cast<std::uint8_t*>(local_combat_actor.load());
  auto local_controller =
      combat_controller_of(local_actor ? *reinterpret_cast<void**>(local_actor + 632) : nullptr);
  // Only a controller of the local fighter's class that belongs to this movement is written.
  if (!request_dodge || !controller || !local_controller ||
      *reinterpret_cast<void**>(controller) != *reinterpret_cast<void**>(local_controller) ||
      *reinterpret_cast<void**>(controller + kControllerMovement) != movement ||
      !*reinterpret_cast<void**>(controller + kControllerInputFilter)) {
    log("COMBAT DODGE replica player=%u cannot dodge here: request=%u controller=%p", player,
        unsigned(request_dodge != nullptr), controller);
    return;
  }
  // The request dodges only with a lock target (core + 3320).
  auto core = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 528);
  if (!core || !*reinterpret_cast<void**>(core + 3320)) {
    log("COMBAT DODGE replica player=%u not started degrees=%.0f: no lock target", player, degrees);
    return;
  }
  const auto keys = controller_vector(controller, kControllerKeys);
  const auto second = controller_vector(controller, kControllerSecondInput);
  set_controller_vector(controller, kControllerKeys, kcds::runtime::combat::dodge_vector(degrees));
  set_controller_vector(controller, kControllerSecondInput, {});
  call_relayed_dodge_request(controller, movement, replica->second.actor);
  set_controller_vector(controller, kControllerKeys, keys);
  set_controller_vector(controller, kControllerSecondInput, second);
  if (!relayed_dodge_made) {
    log("COMBAT DODGE replica player=%u not started degrees=%.0f", player, degrees);
    return;
  }
  // The game plays the dodge's clip on the replica itself: its owner's same clip, coming a moment
  // later with its owner's animations, is not started again.
  open_reaction_window(player);
  log("COMBAT DODGE replica player=%u started degrees=%.0f action=%u windows=%d", player, degrees,
      native_action_type(movement), open_dodge_windows(movement));
}

std::uint64_t local_slot_dodge_at() {
  return last_slot_dodge_at;
}

int local_dodge_windows(void* local_actor) {
  if (!local_actor) {
    return -1;
  }
  return open_dodge_windows(
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(local_actor) + 632));
}

}  // namespace kcds::game_client
