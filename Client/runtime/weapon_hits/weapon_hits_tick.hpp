// The local player's weapon hits each frame: what its swing met goes to the server
// (WEAPON_HIT) and to this machine's resource runtime ({ op: 'weaponHit', hit }, a hit as
// localWorldRay's, physics/local_world.hpp).
#pragma once

#include <functional>

#include "../core/frame.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::game_client {

// Hooks the weapon solver's contact check (weapon_hits/weapon_hits.hpp).
void install_weapon_hits();

// Called by frame() every frame.
void tick_weapon_hits(const Frame& frame);

// Hands the hits to the resource runtime (network thread).
void pump_weapon_hits(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::game_client
