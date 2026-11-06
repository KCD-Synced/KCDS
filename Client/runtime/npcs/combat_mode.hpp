#pragma once

#include <cstdint>
#include <cstring>

namespace kcds::runtime::npcs {

using CombatModeQuery = std::uint8_t (*)(void* combat);

enum class CombatMode { Idle, Active, Unknown };

struct CombatModeObservation {
  CombatMode mode = CombatMode::Idle;
  bool component_present = false;
};

// WHGame 1.5.5: IsInCombatMode calls actor+632's slot1, whose class getter
// reads combat+528 and calls that core's bool-property getter. The caller supplies
// the uniquely resolved class getter. Engine update thread only; no pointer is retained.
inline CombatModeObservation observe_combat_mode(void* actor, CombatModeQuery query) {
  if (!actor) {
    return {};
  }
  std::uint8_t* combat = nullptr;
  std::memcpy(&combat, static_cast<std::uint8_t*>(actor) + 632, sizeof(combat));
  if (!combat) {
    return {};
  }
  CombatModeObservation result{CombatMode::Unknown, true};
  CombatModeQuery* methods = nullptr;
  std::memcpy(&methods, combat, sizeof(methods));
  if (!query || !methods || methods[1] != query) {
    return result;
  }
  // The native getter assumes initialization has completed; an incomplete component must
  // retain the fast publication path without invoking that getter on a missing core.
  void* core = nullptr;
  std::memcpy(&core, combat + 528, sizeof(core));
  if (!core) {
    return result;
  }
  result.mode = query(combat) ? CombatMode::Active : CombatMode::Idle;
  return result;
}

inline bool combat_needs_fast_sync(const CombatModeObservation& observation) {
  return observation.mode != CombatMode::Idle;
}

}  // namespace kcds::runtime::npcs
