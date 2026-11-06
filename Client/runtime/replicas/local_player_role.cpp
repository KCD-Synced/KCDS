#include "local_player_role.hpp"

#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::replicas {
namespace {

// NPC manager: the player actor and its AI object.
constexpr std::size_t kManagerPlayer = 96;
constexpr std::size_t kManagerPlayerAi = 104;
// RegisterNPC clears the player AI global with `mov qword ptr [rip + disp32], 0` at + 0xC3.
constexpr std::size_t kGlobalStore = 0xC3;
constexpr std::uint8_t kGlobalStoreOpcode[] = {0x48, 0xC7, 0x05};
constexpr std::size_t kGlobalStoreLength = 11;

using RegisterNpc = char(__fastcall*)(void* manager, void* actor, unsigned flags);
using RefreshPlayer = std::int64_t(__fastcall*)(void* variables);
using TrackerGet = void*(__fastcall*)();
using TrackerCall = std::int64_t(__fastcall*)(void* tracker);
// The transform tracker's AI object.
constexpr std::size_t kTrackerAi = 32;
using RegisterPlayer = std::int64_t(__fastcall*)(void* registration, void* ai_object);
using ControllerCall = std::int64_t(__fastcall*)(void* controller);

Logger log_line = nullptr;
bool (*spawning)() = nullptr;
RegisterNpc original_register_npc = nullptr;
RegisterPlayer original_register_player = nullptr;
ControllerCall original_register_actions = nullptr;
ControllerCall original_teardown = nullptr;
ControllerCall original_ready = nullptr;
using TriggerPress = std::int64_t(__fastcall*)(void* controller, void* expansion);
TriggerPress original_press = nullptr;
// The shooting expansion's weapon; anything below this is no item.
constexpr std::size_t kExpansionWeapon = 64;
constexpr std::uintptr_t kLowestAddress = 0x10000;
void* (*local_player)() = nullptr;
// A player's missile weapon controller, and the controller's actor.
constexpr std::size_t kPlayerMissileController = 3440;
constexpr std::size_t kControllerActor = 24;
void** player_ai_global = nullptr;
PlayerIdentityNatives identity_natives;

void* field(void* object, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, static_cast<std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

void set_field(void* object, std::size_t offset, void* value) {
  std::memcpy(static_cast<std::uint8_t*>(object) + offset, &value, sizeof(value));
}

// After the role is kept: the transform tracker and "__player" follow the local player again.
void restore_player_identity(void* player_ai) {
  void* tracker = reinterpret_cast<TrackerGet>(identity_natives.tracker)();
  if (tracker && field(tracker, kTrackerAi) != player_ai) {
    reinterpret_cast<TrackerCall>(identity_natives.tracker_unregister)(tracker);
    if (player_ai) {
      reinterpret_cast<TrackerCall>(identity_natives.tracker_register)(tracker);
    }
  }
  void* variables = *identity_natives.variables;
  if (variables && player_ai) {
    reinterpret_cast<RefreshPlayer>(identity_natives.refresh_player)(variables);
  }
  if (log_line) {
    log_line("REPLICA \"__player\" and the transform tracker kept on the local player (ai %p)",
             player_ai);
  }
}

char __fastcall register_npc(void* manager, void* actor, unsigned flags) {
  if (!spawning()) {
    return original_register_npc(manager, actor, flags);
  }
  void* player = field(manager, kManagerPlayer);
  void* player_ai = field(manager, kManagerPlayerAi);
  void* global_ai = *player_ai_global;
  const char result = original_register_npc(manager, actor, flags);
  if (field(manager, kManagerPlayer) != player || field(manager, kManagerPlayerAi) != player_ai) {
    set_field(manager, kManagerPlayer, player);
    set_field(manager, kManagerPlayerAi, player_ai);
    *player_ai_global = global_ai;
    if (log_line) {
      log_line("REPLICA player role kept by the local player (actor %p)", player);
    }
    restore_player_identity(player_ai);
  }
  return result;
}

std::int64_t __fastcall register_player(void* registration, void* ai_object) {
  if (spawning()) {
    if (log_line) {
      log_line("REPLICA player state handler registration of %p skipped", ai_object);
    }
    return 0;
  }
  return original_register_player(registration, ai_object);
}

// Whether a controller belongs to a player other than the game's local one.
bool foreign(void* controller) {
  void* local = local_player();
  return local && field(controller, kControllerActor) != local;
}

std::int64_t __fastcall register_actions(void* controller) {
  if (foreign(controller)) {
    if (log_line) {
      log_line("REPLICA missile weapon controller %p takes no input", controller);
    }
    return 0;
  }
  return original_register_actions(controller);
}

std::int64_t __fastcall ready(void* controller) {
  if (foreign(controller)) {
    return 0;
  }
  return original_ready(controller);
}

std::int64_t __fastcall press(void* controller, void* expansion) {
  const auto weapon =
      expansion ? reinterpret_cast<std::uintptr_t>(field(expansion, kExpansionWeapon)) : 0;
  if (foreign(controller) || weapon < kLowestAddress) {
    if (log_line) {
      log_line("REPLICA missile trigger of controller %p ignored (weapon %p)", controller,
               reinterpret_cast<void*>(weapon));
    }
    return 0;
  }
  return original_press(controller, expansion);
}

std::int64_t __fastcall teardown(void* controller) {
  const bool replica = foreign(controller);
  const std::int64_t result = original_teardown(controller);
  void* local = local_player();
  if (replica && local) {
    void* own = static_cast<std::uint8_t*>(local) + kPlayerMissileController;
    if (field(own, kControllerActor) == local) {
      original_register_actions(own);
      if (log_line) {
        log_line("REPLICA missile weapon input given back to the local player");
      }
    }
  }
  return result;
}

bool hook(void* target, void* detour, void** original) {
  return MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool keep_local_player_role(void* register_npc_target, void* register_player_handler,
                            const PlayerIdentityNatives& identity, bool (*spawning_replica)(),
                            Logger logger) {
  log_line = logger;
  spawning = spawning_replica;
  identity_natives = identity;
  if (!register_npc_target || !register_player_handler || !spawning) {
    return false;
  }
  if (!identity.variables || !identity.refresh_player || !identity.tracker ||
      !identity.tracker_register || !identity.tracker_unregister) {
    return false;
  }
  const auto* store = static_cast<const std::uint8_t*>(register_npc_target) + kGlobalStore;
  if (std::memcmp(store, kGlobalStoreOpcode, sizeof(kGlobalStoreOpcode)) != 0) {
    return false;
  }
  std::int32_t displacement = 0;
  std::memcpy(&displacement, store + sizeof(kGlobalStoreOpcode), sizeof(displacement));
  player_ai_global = reinterpret_cast<void**>(const_cast<std::uint8_t*>(store) +
                                              kGlobalStoreLength + displacement);
  return hook(register_npc_target, reinterpret_cast<void*>(register_npc),
              reinterpret_cast<void**>(&original_register_npc)) &&
         hook(register_player_handler, reinterpret_cast<void*>(register_player),
              reinterpret_cast<void**>(&original_register_player));
}

bool keep_local_missile_input(void* register_target, void* teardown_target, void* ready_target,
                              void* press_target, void* (*local)()) {
  local_player = local;
  return register_target && teardown_target && ready_target && press_target && local_player &&
         hook(ready_target, reinterpret_cast<void*>(ready),
              reinterpret_cast<void**>(&original_ready)) &&
         hook(press_target, reinterpret_cast<void*>(press),
              reinterpret_cast<void**>(&original_press)) &&
         hook(register_target, reinterpret_cast<void*>(register_actions),
              reinterpret_cast<void**>(&original_register_actions)) &&
         hook(teardown_target, reinterpret_cast<void*>(teardown),
              reinterpret_cast<void**>(&original_teardown));
}

}  // namespace kcds::runtime::replicas
