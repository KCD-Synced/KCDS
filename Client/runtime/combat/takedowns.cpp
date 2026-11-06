// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "takedowns.hpp"

#include <cstddef>
#include <cstdint>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../npcs/native_npcs.hpp"
#include "../replicas/remote.hpp"
#include "MinHook.h"

namespace kcds::game_client {
namespace {

// The takedown's target entity id.
constexpr std::size_t kTakedownTarget = 120;

using TakedownStart = char(__fastcall*)(void* takedown);
TakedownStart original_takedown_start = nullptr;

// Whether the entity is a body KCDS moves: another player's or a server NPC's.
bool kcds_body(std::uint32_t engine_id) {
  if (engine_id == 0) {
    return false;
  }
  for (const auto& [id, remote] : remotes) {
    if (remote.engine_id == engine_id) {
      return true;
    }
  }
  for (const NpcBody& npc : npc_bodies()) {
    if (npc.engine_id == engine_id) {
      return true;
    }
  }
  return false;
}

char __fastcall hooked_takedown_start(void* takedown) {
  if (takedown) {
    const std::uint32_t target = *reinterpret_cast<const std::uint32_t*>(
        static_cast<std::uint8_t*>(takedown) + kTakedownTarget);
    if (kcds_body(target)) {
      log("COMBAT takedown on body %u refused (a player's or a server NPC's)", target);
      return 0;
    }
  }
  return original_takedown_start(takedown);
}

}  // namespace

bool install_takedown_guard() {
  void* start = resolve_unique(
      "TakedownStart",
      "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 48 8D 68 98 48 81 EC 60 01 00 00 48 8B 05 "
      "?? ?? ?? ?? 48 33 C4 48 89 45 50 48 8B D9 C6 44 24 20 01 E8 ?? ?? ?? ?? 8B 53 78 48 8B 88 "
      "80 01 00 00");
  return start &&
         MH_CreateHook(start, reinterpret_cast<void*>(hooked_takedown_start),
                       reinterpret_cast<void**>(&original_takedown_start)) == MH_OK &&
         MH_EnableHook(start) == MH_OK;
}

}  // namespace kcds::game_client
