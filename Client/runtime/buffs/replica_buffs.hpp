// The owner's fighting buffs on its replicas. A hit is evaluated by the victim's
// game, and its attack value reads the attacker's soul there, the replica's, buff modifiers
// included, so a replica carries its owner's potions, drinks and
// injuries (kcd2s/integration/replica_buffs.hpp names the classes).
//
//   a soul's buffs  soul + 1352 .. + 1360, a vector of instances: + 8 the instance's id, + 16 its
//                   definition (+ 0 the GUID, + 32 the class id), as player_buffs.hpp reads them
//   the manager     C_BuffManager (getter): slot 0 adds a buff (manager, soul, &GUID,
//                   init parameters, &perk) and returns the instance, slot 3 removes one instance
//                   (manager, &instance id)
//   an injury       C_InjuredSoulBuffInstance (init): + 216 its severity (1/32768),
//                   + 220 its type, + 232 / + 244 its two flags, which its init takes from the
//                   C_InjuredBuffInitParams it is added with (duration, type,
//                   flags); a hit's injury comes with those, a plain add of it does not hold
//
// A buff added on a replica runs its own course there; apply() puts back what differs from the
// owner's report, one instance per instance the owner has, an injury with its owner's numbers.
//
// Engine thread.
#pragma once

#include <vector>

#include "kcd2s/integration/replica_buffs.hpp"

namespace kcds::runtime::buffs::replica {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* manager = nullptr;                // the buff manager's getter
  void* injured_init = nullptr;           // the injury instance's init
  void* create_injured_params = nullptr;  // C_InjuredBuffInitParams
};

// False when the manager is missing (without the injury natives injuries are added plainly).
bool install(const Natives& natives, Logger logger);

// Every instance of a mirrored class on the soul (the local player's), sorted, an injury with
// its numbers (severity in whole kInjurySeverityStep steps); at most kMaxReplicaBuffs.
std::vector<kcd2s::integration::ReplicaBuff> read(void* soul);

// What apply() did on one replica's soul for one report of its owner's; a new report or soul
// starts a new one.
struct Mirror {
  std::vector<kcd2s::integration::Guid> added;        // added by the last apply()
  std::vector<kcd2s::integration::Guid> not_holding;  // added and gone again: not added again
};

// Gives a replica's soul its owner's instances of the mirrored classes: adds the missing ones,
// sets an injury's numbers to its owner's when they moved apart by a step or more, and removes
// those the owner does not have. A buff that does not hold without the parameters it came with
// on the owner (a drink's, for one) is added once per report. The number of instances added,
// changed and removed.
unsigned apply(void* replica_soul, const std::vector<kcd2s::integration::ReplicaBuff>& owner,
               Mirror& mirror);

}  // namespace kcds::runtime::buffs::replica
