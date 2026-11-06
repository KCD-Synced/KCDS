#pragma once

#include "kcd2s/integration/progression.hpp"

namespace kcd2s::integration {

// WHGame 1.5.5 Riposte ability perk, confirmed against the native ability-2
// registration and the existing game-data catalogue. This is a perk identity,
// never a perfect-block outcome or an instruction to open a timing window.
inline constexpr PerkId kRiposteAbilityPerk{0x74, 0x52, 0x4c, 0xec, 0xe3, 0x50, 0xbf, 0x4b,
                                            0x92, 0x20, 0x82, 0x3b, 0x08, 0x06, 0x47, 0xc4};

inline bool owns_riposte_ability(const Progression& progression) {
  return std::binary_search(progression.perks.begin(), progression.perks.end(),
                            kRiposteAbilityPerk);
}

}  // namespace kcd2s::integration
