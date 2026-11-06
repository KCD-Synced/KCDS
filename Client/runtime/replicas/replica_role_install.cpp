// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_role_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "local_player_role.hpp"
#include "replica_spawn.hpp"
#include "remote_interactor_lifetime.hpp"
#include "remote_ui.hpp"

namespace kcds::game_client {

void install_replica_role() {
  if (!install_remote_ui_role() || !install_remote_interactor_lifetime()) {
    failed = true;
    return;
  }
  // Replicas are made without taking the player role from the local player, nor the
  // "__player" variable and the transform tracker. RegisterNPC's player branch: the
  // player's AI object made, the variable table loaded, the manager's player set, "__player"
  // refreshed, then the tracker's singleton and register.
  constexpr const char* kPlayerBranch =
      "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 49 89 46 10 49 89 45 68 49 8B 46 10 48 89 05 ?? ?? ?? "
      "?? E8 ?? ?? ?? ?? EB 09 E8 ?? ?? ?? ?? 49 89 46 10 49 83 7E 10 00 75 07 32 C0 E9 ?? ?? ?? "
      "?? 4D 85 FF 74 0D E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ??";
  kcds::runtime::replicas::PlayerIdentityNatives identity;
  identity.variables =
      static_cast<void**>(resolve_unique("AiGlobalVariables", kPlayerBranch, 8, 12));
  identity.refresh_player = resolve_unique("AiRefreshPlayerVariable", kPlayerBranch, 32, 36);
  identity.tracker = resolve_unique("AiTransformTracker", kPlayerBranch, 67, 71);
  identity.tracker_register = resolve_unique("AiTransformTrackerRegister", kPlayerBranch, 75, 79);
  identity.tracker_unregister = resolve_unique(
      "AiTransformTrackerUnregister", "40 53 48 83 EC 40 48 8B D9 E8 ?? ?? ?? ?? 48 8D 0D");
  const bool kept = kcds::runtime::replicas::keep_local_player_role(
      resolve_unique("NpcManagerRegisterNpc",
                     "48 89 5C 24 18 55 56 57 41 56 41 57 48 8B EC 48 83 EC 50 8A 41 28"),
      resolve_unique("PlayerStateHandlerRegister",
                     "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57 48 8B EC 48 83 "
                     "EC 30 C6 41 02 01"),
      identity, [] { return spawning_remote; }, log);
  log("REPLICA player role %s", kept ? "kept by the local player" : "hooks unavailable");
  // Bows, crossbows and guns: only the local player's controller takes their input.
  const bool missile = kcds::runtime::replicas::keep_local_missile_input(
      resolve_unique("MissileControllerActions",
                     "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 56 41 57 48 8D 68 A1 "
                     "48 81 EC A0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 37 48 8B F1"),
      resolve_unique("MissileControllerTeardown",
                     "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 30 48 8B F9 48 8B 49 18"),
      resolve_unique("MissileControllerReady",
                     "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 EC "
                     "20 48 8B F9 48 8B 49 18"),
      resolve_unique("MissileTriggerPress", "40 53 48 83 EC 20 48 8B DA C6 41 20 01"),
      []() -> void* {
        // The game's local player: GameInterface slot 64 (see frame()).
        auto services = get_services();
        auto game_interface =
            services ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + 8)
                     : nullptr;
        return game_interface ? slot<void*(__fastcall*)(void*)>(game_interface, 64)(game_interface)
                              : nullptr;
      });
  log("REPLICA missile input %s", missile ? "kept by the local player" : "hook unavailable");
}

}  // namespace kcds::game_client
