// The local player's progression, read off its soul and
// reported to the server (PROGRESSION, kcd2s/integration/progression.hpp), which keeps it and
// answers the resources' Player.levels, perks, progression and trackOf.
//
// The soul (actor + 1640, WHGame 1.5.5):
//
//   soul + 1512 + 8 * skill   a skill: its level (int), then the XP towards the next level
//                             (int, fixed point, 128 per XP)
//   soul + 1388 + 8 * stat    a stat, the same pair
//   soul + 1880               the perk set: the owned perk instances at + 64 .. + 72 (a vector
//                             of pointers; an instance's GUID at + 16, the perks it grants with
//                             it at its vtable slot 5, a vector of instances: the game's own
//                             "owns" counts them too); its + 88 is the perk
//                             points: 16-bit counters, stats at + 16 + 2 * stat, skills at
//                             + 36 + 2 * skill, the main level's at + 106 (the game's level-up
//                             listener adds to them)
//
// Engine thread.
#pragma once

#include "kcd2s/integration/progression.hpp"

namespace kcds::runtime::progression {

// The progression of the soul; false when it cannot be read (no soul, an implausible perk set).
bool read(void* soul, kcd2s::integration::Progression& progression);

}  // namespace kcds::runtime::progression
