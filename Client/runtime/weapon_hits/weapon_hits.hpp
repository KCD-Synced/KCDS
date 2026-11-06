// The local player's swing meeting something solid that is not a body (issue):
// the client's weaponHit event and the server's playerWeaponHit (weapon_hits.hpp
// WEAPON_HIT).
//
// The game's weapon solver (wh::combatmodule, its per-frame update) asks its contact
// check every frame while an attack's blade is live, and only then: from the swing's
// first live frame to its last. The check itself only meets other fighters' weapon and body
// colliders, never the level (a sword swung into a wall found nothing), so the
// level is met here: the hook notes that the local player's blade is live this frame, and each
// frame of a live blade traces where the blade went since the frame before (its tip's path and
// the blade itself, from the hand to the tip) with the game's own RayWorldIntersection in the
// cover set (terrain, static geometry and rigid bodies; living bodies are never met, the
// player's own physics and the weapon's skipped).
//
//   the solver's context   *checker; its owner, the actor that swings, at its + 0
//   the weapon             the item in the actor's right hand (weapons/drawn_weapons.hpp), its
//                          entity's GetWorldTM (slot 32); the blade along the entity's local +Z,
//                          as far as its render component's local bounds reach (slot 18, min then
//                          max)
//
// A thing is reported once per attack, at the first contact: its collider (terrain: the terrain
// once). An attack is the run of live frames one solver check object asks for, a gap of more than
// kAttackGap seconds starting the next. Fists and kicks hold no weapon in the right hand and are
// not reported.
//
// Hook: the game's thread that updates the solver. Tick and taking: the engine thread.
#pragma once

#include <cstdint>
#include <vector>

#include "../physics/line_of_sight.hpp"

namespace kcds::runtime::weapon_hits {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* contact_check = nullptr;  // the weapon solver's contact check
  void* component = nullptr;      // an entity's component by its id
};

bool install(const Natives& natives, Logger logger);

// One thing the blade met: where and what, with the distance from the player's feet.
struct Contact {
  physics::RayHit hit;
  float distance = 0.0f;
};

// Traces the local player's blade for this frame. actor: the local player's actor, entity its
// entity, weapon the entity of the item in its right hand (null: nothing is traced, the attack
// still ends). Returns the things met for the first time in this attack.
std::vector<Contact> tick(void* actor, void* entity, void* weapon, double now);

}  // namespace kcds::runtime::weapon_hits
