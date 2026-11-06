#include "../core/client_internal.hpp"

#include "women_targets.hpp"

#include <cstdint>
#include <mutex>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::game_client {
namespace {

// The lock-on's exclusion check (controller, actor): true leaves the body out.
using LockExclusion = bool(__fastcall*)(void* controller, void* actor);
// C_Soul::HasGender (soul, gender): the soul's archetype is of that gender.
using SoulHasGender = bool(__fastcall*)(void* soul, std::uint8_t gender);

LockExclusion original_lock_exclusion = nullptr;
SoulHasGender original_soul_has_gender = nullptr;

constexpr std::uint8_t kFemale = 2;

// Set on the thread that runs the exclusion check, for the check alone.
thread_local bool in_lock_exclusion = false;

bool remote_player_soul(void* soul) {
  std::lock_guard<std::mutex> lock(combat_identity_mutex);
  return remote_combat_souls.count(soul) != 0;
}

bool __fastcall hooked_lock_exclusion(void* controller, void* actor) {
  const bool previous = in_lock_exclusion;
  in_lock_exclusion = true;
  const bool excluded = original_lock_exclusion(controller, actor);
  in_lock_exclusion = previous;
  return excluded;
}

bool __fastcall hooked_soul_has_gender(void* soul, std::uint8_t gender) {
  const bool has = original_soul_has_gender(soul, gender);
  if (has && in_lock_exclusion && gender == kFemale && remote_player_soul(soul)) {
    return false;
  }
  return has;
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install_women_targets() {
  void* const lock_exclusion = resolve_unique(
      "LockOnExclusion",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 48 8B F1 0F 29 74 24 50 48 8B CA 48 8B DA "
      "E8 ?? ?? ?? ?? 84 C0 0F 85 ?? ?? ?? ?? 48 8B 03 48 8B CB");
  void* const has_gender = resolve_unique(
      "SoulHasGender", "48 8B 81 B8 0C 00 00 38 50 1C 75 03 B0 01 C3 84 50 1C 75 F8 32 C0 C3");
  const bool installed = hook(lock_exclusion, reinterpret_cast<void*>(&hooked_lock_exclusion),
                              reinterpret_cast<void**>(&original_lock_exclusion)) &&
                         hook(has_gender, reinterpret_cast<void*>(&hooked_soul_has_gender),
                              reinterpret_cast<void**>(&original_soul_has_gender));
  log("COMBAT women players as lock-on targets: %s", installed ? "on" : "not found");
  return installed;
}

}  // namespace kcds::game_client
