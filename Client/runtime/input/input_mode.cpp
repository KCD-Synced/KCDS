// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "input_mode.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../knockout/replica_wake_tick.hpp"

#include <intrin.h>

namespace kcds::game_client {

// MCP apply actor-owned native
// interactive state, but also change the singleton ActionMapManager. A remote
// get-up must retain its own restriction/animation and never disable local keys.
core::EntityId remote_combat_id(void* movement);
thread_local void* input_mode_actor = nullptr;
struct InputModeOwner {
  void* previous;
  explicit InputModeOwner(void* actor) : previous(input_mode_actor) {
    input_mode_actor = actor;
  }
  ~InputModeOwner() {
    input_mode_actor = previous;
  }
};
InteractiveState original_interactive_state = nullptr;
NativeMoveToStart original_move_to_start = nullptr;
NativeMoveToEnd original_move_to_end = nullptr;
EnableActionMap original_enable_action_map = nullptr;
// The singleton CActionMapManager, captured from its own EnableActionMap calls.
std::atomic<void*> action_map_manager{nullptr};
std::intptr_t __fastcall hooked_interactive_state(void* self, void* out, void* actor,
                                                  const void* event) {
  InputModeOwner owner(actor);
  return original_interactive_state(self, out, actor, event);
}
double __fastcall hooked_move_to_start(void* self) {
  InputModeOwner owner(*reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 56));
  return original_move_to_start(self);
}
void __fastcall hooked_move_to_end(void* self, unsigned request, char result) {
  InputModeOwner owner(*reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 56));
  original_move_to_end(self, request, result);
}
// C_PlayerStateHandler pushes and pops its player's non-interactive
// counter (+718) and, on the first push and the last pop, switches the game's single global
// "noninteractive" action map. Every C_Player has such a handler (constructor),
// including the bodies of remote players, so a replica entering a handler state (right
// after a hit in a fist fight) disabled the local player's camera, movement and attacks for 7-8 s
// The handler's player is *(this+576)->vtbl[29](); for a replica the
// counter still changes but the global action map is left alone.
PlayerStateNoninteractive original_player_state_push = nullptr;
PlayerStateNoninteractive original_player_state_pop = nullptr;
thread_local bool replica_player_state = false;
thread_local void* player_state_owner = nullptr;
bool replica_player_state_handler(void* handler) {
  auto owner_proxy = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(handler) + 576);
  player_state_owner =
      owner_proxy ? slot<void*(__fastcall*)(void*)>(owner_proxy, 29)(owner_proxy) : nullptr;
  return player_state_owner &&
         *reinterpret_cast<void***>(player_state_owner) == remote_player_table + 1;
}
void __fastcall hooked_player_state_push(void* handler) {
  const bool previous = replica_player_state;
  replica_player_state = replica_player_state_handler(handler);
  original_player_state_push(handler);
  replica_player_state = previous;
  player_state_owner = nullptr;
}
void __fastcall hooked_player_state_pop(void* handler) {
  const bool previous = replica_player_state;
  replica_player_state = replica_player_state_handler(handler);
  original_player_state_pop(handler);
  replica_player_state = previous;
  player_state_owner = nullptr;
}
void __fastcall hooked_enable_action_map(void* self, const char* name, bool enabled) {
  action_map_manager.store(self);
  if (name && std::strcmp(name, "loading") == 0) {
    loading_down_since_request.store(!enabled);
    log("LOADING action map enabled=%u tick=%llu", unsigned(enabled), GetTickCount64());
  }
  if (name && std::strcmp(name, "noninteractive") == 0) {
    if (replica_player_state) {
      log("CONTROL replica state handler noninteractive omitted enabled=%u tick=%llu",
          unsigned(enabled), GetTickCount64());
      return;
    }
    auto movement =
        input_mode_actor
            ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(input_mode_actor) + 632)
            : nullptr;
    const auto remote = remote_combat_id(movement);
    const bool remote_role =
        input_mode_actor && *reinterpret_cast<void***>(input_mode_actor) == remote_player_table + 1;
    if (remote || remote_role) {
      log("CONTROL remote noninteractive omitted player=%u enabled=%u tick=%llu", remote,
          unsigned(enabled), GetTickCount64());
      return;
    }
    log("CONTROL local noninteractive enabled=%u owner=%p state_owner=%p local=%p caller=%p "
        "tick=%llu",
        unsigned(enabled), input_mode_actor, player_state_owner, local_combat_actor.load(),
        _ReturnAddress(), GetTickCount64());
  }
  original_enable_action_map(self, name, enabled);
}

// MCP is the original model context setter. Its 21
// individual bool scopes start at +24; +45 is their native aggregate.
// Observe changes and caller stacks; never write or clear any scope ourselves.
SetMovementRestriction original_set_movement_restriction = nullptr;
void observe_replica_wake_restriction(core::EntityId player, void* gate);
std::intptr_t __fastcall hooked_set_movement_restriction(void* self, unsigned char scope,
                                                         char enabled) {
  auto gate = static_cast<std::uint8_t*>(self);
  const bool changed = scope < 21 && gate[24 + scope] != static_cast<unsigned char>(enabled);
  auto result = original_set_movement_restriction(self, scope, enabled);
  if (!changed)
    return result;
  auto own = static_cast<std::uint8_t*>(local_combat_actor.load());
  const bool local = own && *reinterpret_cast<std::uint8_t**>(own + 2448) + 920 == gate;
  core::EntityId id = 0;
  if (!local) {
    std::lock_guard<std::mutex> lock(combat_identity_mutex);
    for (const auto& [soul, movement] : remote_combat_movements) {
      auto actor = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(movement) + 504);
      if (actor && *reinterpret_cast<std::uint8_t**>(actor + 2448) + 920 == gate) {
        if (auto it = remote_combat_souls.find(soul); it != remote_combat_souls.end())
          id = it->second;
        break;
      }
    }
  }
  if (!local && !id)
    return result;
  if (id && (scope == 7 || scope == 9)) {
    observe_replica_wake_restriction(id, gate);
  }
  unsigned mask = 0;
  for (unsigned i = 0; i < 21; ++i)
    if (gate[24 + i])
      mask |= 1u << i;
  void* stack[12]{};
  const auto count = CaptureStackBackTrace(1, 12, stack, nullptr);
  log("CONTROL restriction local=%u source=%u scope=%u enabled=%u aggregate=%u mask=%x tick=%llu",
      unsigned(local), id, unsigned(scope), unsigned(enabled), unsigned(gate[45]), mask,
      GetTickCount64());
  for (unsigned i = 0; i < count; ++i) {
    const auto at = reinterpret_cast<std::uintptr_t>(stack[i]);
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    if (at >= base && at - base < image_size)
      log("CONTROL restriction caller frame=%u ida=%llx", i, 0x180000000ULL + at - base);
  }
  return result;
}

}  // namespace kcds::game_client
