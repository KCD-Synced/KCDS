// The owner's RPG soul on its replicas. A hit is evaluated by the victim's game, and
// the game's attack value reads the attacker's soul (WHGame 1.5.5: strength against
// the weapon's strength requirement, and the weapon skill, GetSkillLevel),
// so on another client the attacker's replica must have the owner's levels and perks.
//
//   levels  the track pairs (progression_report.hpp): GetStatLevel and GetSkillLevel
//           read the level at soul + 1388 / 1512 + 8 * id and add the soul's modifiers. The game
//           has no level setter (it only adds XP, which cannot lower a level), so a replica's
//           level is set where the game's save loader puts it.
//   perks   the perk set's directly owned perks: removed and added as
//           the progression orders do; the perks those grant come with them.
//
// Engine thread.
#pragma once

#include "kcd2s/integration/replica_soul.hpp"

namespace kcds::runtime::progression::replica_soul {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* add_perk = nullptr;
  void* remove_perk = nullptr;
};

// False when a native is missing.
bool install(const Natives& natives, Logger logger);

// The local player's levels and directly owned perks; false when the soul cannot be read.
bool read(void* soul, kcd2s::integration::ReplicaSoul& soul_state);

// Gives a replica's soul its owner's levels and perks, once per change of the owner's report.
// False when a perk the owner has could not be added.
bool apply(void* replica_soul, const kcd2s::integration::ReplicaSoul& owner);

}  // namespace kcds::runtime::progression::replica_soul
