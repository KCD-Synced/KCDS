// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "replica_targets.hpp"

#include <cstdint>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"
#include "remote_lifecycle.hpp"
#include "replica_autonomy.hpp"
#include "replica_spawn.hpp"

namespace kcds::game_client {

void resolve_replica_targets(StartupTargets& targets) {
  auto movement_request = resolve_unique("AnimatedMovementRequest",
                                         "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 41 54 41 "
                                         "55 41 56 41 57 48 83 EC 40 33 ED 4C 8B F2 48 8B F1");
  auto ai_object_ctor =
      resolve_unique("AIObjectCtor",
                     "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 30 48 8B 1D ?? ?? ?? ?? 48 8D 05 "
                     "?? ?? ?? ?? 48 89 01 48 8B FA 48 8B 02 48 8B F1 48 89 41 08 83 C8 FF");
  auto ai_object_dtor =
      resolve_unique("AIObjectDtor",
                     "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 48 8B F9 "
                     "48 89 01 E8 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00");
  cancel_transform = reinterpret_cast<CancelTransform>(resolve_unique(
      "CancelTransformSubscription",
      "48 8B C4 48 89 58 18 48 89 70 20 48 89 50 10 55 57 41 56 48 8B EC 48 81 EC 80 00 00 00 48 "
      "8B F1 C6 40 88 00 48 8D 4D C0 E8 ?? ?? ?? ?? 80 7E 38 00 4C 8D 76 08"));
  transform_manager_slot = static_cast<void**>(
      resolve_unique("AITransformManagerSlot",
                     "48 89 5C 24 08 55 56 57 48 8D 6C 24 B9 48 81 EC A0 00 00 00 48 8B 1D ?? ?? "
                     "?? ?? 48 8B F2 48 8B F9 90 48 85 DB 0F 85 4C 01 00 00 48 8D 0D ?? ?? ?? ?? "
                     "FF 15 ?? ?? ?? ?? 85 C0 74 0A 8D 4B",
                     23, 27));
  // The AI module's actor grid and its removal (see hooked_ai_object_dtor).
  ai_module_slot = static_cast<std::uint8_t**>(
      resolve_unique("AIModuleSlot", "48 8B 05 ?? ?? ?? ?? 48 8B 40 78 C3", 3, 7));
  ai_grid_remove = reinterpret_cast<AIGridRemove>(resolve_unique(
      "AIGridRemove", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8D B1 B8 01"));
  targets.movement_request = movement_request;
  targets.ai_object_ctor = ai_object_ctor;
  targets.ai_object_dtor = ai_object_dtor;
}

DWORD install_replica_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const movement_request = targets.movement_request;
  void* const ai_object_ctor = targets.ai_object_ctor;
  void* const ai_object_dtor = targets.ai_object_dtor;
  if (MH_CreateHook(movement_request, reinterpret_cast<void*>(hooked_movement_request),
                    reinterpret_cast<void**>(&original_movement_request)) != MH_OK)
    return 1;
  if (MH_EnableHook(movement_request) != MH_OK)
    return 1;
  if (MH_CreateHook(ai_object_ctor, reinterpret_cast<void*>(hooked_ai_object_ctor),
                    reinterpret_cast<void**>(&original_ai_object_ctor)) != MH_OK)
    return 1;
  if (MH_EnableHook(ai_object_ctor) != MH_OK)
    return 1;
  if (MH_CreateHook(ai_object_dtor, reinterpret_cast<void*>(hooked_ai_object_dtor),
                    reinterpret_cast<void**>(&original_ai_object_dtor)) != MH_OK)
    return 1;
  if (MH_EnableHook(ai_object_dtor) != MH_OK)
    return 1;
  return 0;
}

DWORD install_replica_autonomy() {
  native_npc_lookup = reinterpret_cast<NativeNpcLookup>(resolve_unique(
      "NativeNpcByEntityId",
      "48 89 5C 24 10 57 48 83 EC 30 48 8B 41 38 48 8B FA 4C 8B 41 30 48 8B D9 48 85 C0 74 "
      "77 48 8D 04 40"));
  native_npc_actor = reinterpret_cast<NativeNpcActor>(
      resolve_unique("NativeNpcActor", "48 8B 49 18 48 8B 01 48 FF A0 A8 00 00 00"));
  auto autonomy_update = resolve_unique(
      "NativeNpcAutonomyUpdate",
      "48 89 5C 24 18 48 89 6C 24 20 56 57 41 56 48 81 EC B0 00 00 00 48 8B 05 ?? ?? ?? ?? "
      "48 33 C4 48 89 84 24 A0 00 00 00 48 8B F9 4C 8B F2");
  if (!native_npc_lookup || !native_npc_actor || !autonomy_update ||
      MH_CreateHook(autonomy_update, reinterpret_cast<void*>(hooked_autonomy_update),
                    reinterpret_cast<void**>(&original_autonomy_update)) != MH_OK ||
      MH_EnableHook(autonomy_update) != MH_OK) {
    log("Remote autonomy signatures unavailable; adapter not started");
    return 1;
  }
  return 0;
}

}  // namespace kcds::game_client
