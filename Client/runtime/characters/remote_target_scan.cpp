// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "remote_target_scan.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::game_client {

namespace {

// The list's +0 is the owner's combat object (actor +632); its +504 is the C_Actor, whose soul is
// at +1640 (combat_identity).
constexpr std::size_t kCombatActor = 504;
constexpr std::size_t kActorSoul = 1640;
// The list of candidates: begin at +8, end at +16 (a std::vector of the object).
constexpr std::size_t kListBegin = 8;
constexpr std::size_t kListEnd = 16;
// The settings object's "walk every actor" switch, off selecting the area query below.
constexpr std::size_t kWalkEveryActor = 0x268;
// The offset of the settings getter's call in the scan, the rel32 after its E8.
constexpr std::size_t kSettingsCallRel = 62;
constexpr std::size_t kSettingsCallEnd = 66;

using Scan = std::int64_t(__fastcall*)(void* list, float seconds);
using AreaQuery = std::int64_t(__fastcall*)(void* list);
using Settings = std::uint8_t*(__fastcall*)();

Scan original_scan = nullptr;
AreaQuery original_area_query = nullptr;
Settings settings = nullptr;
thread_local bool skipping_scan = false;

bool owned_by_replica(void* list) {
  void* combat = *static_cast<void**>(list);
  if (!combat) {
    return false;
  }
  void* actor = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(combat) + kCombatActor);
  if (!actor) {
    return false;
  }
  void* soul = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actor) + kActorSoul);
  return soul && is_replica_soul(soul);
}

std::int64_t __fastcall hooked_scan(void* list, float seconds) {
  if (!owned_by_replica(list)) {
    return original_scan(list, seconds);
  }
  // The rest of the function (the aim point) still runs: only its walk is switched off, for this
  // one call on this thread, and the query it then takes instead does nothing.
  auto& walk_every_actor = *reinterpret_cast<std::int32_t*>(settings() + kWalkEveryActor);
  const std::int32_t saved = walk_every_actor;
  walk_every_actor = 0;
  skipping_scan = true;
  const std::int64_t result = original_scan(list, seconds);
  skipping_scan = false;
  walk_every_actor = saved;
  return result;
}

std::int64_t __fastcall hooked_area_query(void* list) {
  if (!skipping_scan) {
    return original_area_query(list);
  }
  auto* bytes = static_cast<std::uint8_t*>(list);
  *reinterpret_cast<void**>(bytes + kListEnd) = *reinterpret_cast<void**>(bytes + kListBegin);
  return 0;
}

}  // namespace

bool install_remote_target_scan() {
  auto* scan =
      static_cast<std::uint8_t*>(resolve_unique("PlayerTargetScan",
                                                "48 8B C4 48 89 58 10 55 56 57 41 56 41 57 48 8D "
                                                "68 A1 48 81 EC D0 00 00 00 0F 29 70 C8 48 8B "
                                                "F9 0F 29 78 B8 44 0F 29 40 A8 44 0F 29 48 98 44 "
                                                "0F 28 C9 44 0F 29 50 88 44 0F 29 5C 24 70 E8 "
                                                "?? ?? ?? ?? 33 DB 39 98 68 02 00 00"));
  void* area_query = resolve_unique("PlayerTargetAreaQuery",
                                    "48 8B C4 48 89 58 20 57 48 81 EC 20 01 00 00 0F 29 70 E8 48 "
                                    "8B F9 0F 29 78 D8 44 0F 29 40 C8 "
                                    "44 0F 29 48 B8 48 8B 41 08 48 3B 41 10 74 04 48 89");
  if (!scan || !area_query) {
    log("CHARACTERS remote target scan: a function was not found, the game keeps its own");
    return false;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, scan + kSettingsCallRel, sizeof(relative));
  settings = reinterpret_cast<Settings>(scan + kSettingsCallEnd + relative);
  const bool hooked = MH_CreateHook(scan, reinterpret_cast<void*>(hooked_scan),
                                    reinterpret_cast<void**>(&original_scan)) == MH_OK &&
                      MH_CreateHook(area_query, reinterpret_cast<void*>(hooked_area_query),
                                    reinterpret_cast<void**>(&original_area_query)) == MH_OK &&
                      MH_EnableHook(scan) == MH_OK && MH_EnableHook(area_query) == MH_OK;
  log("CHARACTERS remote target scan %s", hooked ? "installed" : "failed");
  return hooked;
}

}  // namespace kcds::game_client
