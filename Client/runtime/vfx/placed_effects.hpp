// The particle effects the server places (kcd2s/integration/placed_objects.hpp).
//
// An effect is a BasicEntity at the order's pose carrying one particle emitter of the game's
// effect library, loaded as the entity binding's LoadParticleEffect does (the particle manager's
// FindEffect, then CEntity::LoadParticleEmitter with the six tuning values in the emitter's spawn
// parameters). The entity is named after the effect's server id, so an update rebuilds it and a
// destroy removes it. An update that changes only the scale keeps the running emitter and scales
// its entity instead (CEntity::SetScale; the emitter follows its slot's transform), so a fire that
// grows does not start over at every step. A burst plays once on an entity of its own
// (or attached to a body of another player or NPC, which it follows) and the client removes that
// entity when the burst's duration is over; the server sends no end. An effect on a player's or
// NPC's body (a burning torch in a hand) hangs on the body or its joint at the order's
// offset, as a burst on a body does; one whose body is not in this game yet comes when it does,
// and one whose body was rebuilt is hung again.
//
// Engine thread.
#pragma once

#include <cstdint>

#include "kcd2s/integration/placed_objects.hpp"

namespace kcds::runtime::vfx::placed {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* const* particle_manager = nullptr;  // the slot of the game's particle manager
  void* spawn_params_init = nullptr;        // the emitter's spawn parameters constructor
  void* load_emitter = nullptr;             // CEntity::LoadParticleEmitter
  void* attach_child = nullptr;             // CEntity::AttachChild(parent, child, params)
  // The game entity of a replicated body (a player's or an NPC's) by its network id, or null.
  void* (*body_of)(std::uint32_t network_id) = nullptr;
  Logger log = nullptr;
};

// False when a native is missing; no effect is then shown.
bool install(const Natives& natives);

// Spawns, rebuilds or removes an effect, or plays a burst.
void apply(const kcd2s::integration::VfxOrder& order);

// Removes the bursts whose duration is over, and hangs the effects on bodies that came or were
// rebuilt. Called every frame.
void tick();

// Forgets every effect and burst, as the game's level does when it unloads.
void reset();

}  // namespace kcds::runtime::vfx::placed
