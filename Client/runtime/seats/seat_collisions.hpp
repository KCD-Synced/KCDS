// Seated players do not push or block each other's bodies.
//
// A replica is a living body with the game's collision capsule. While two players sat at a dice
// table, a third player walking into them pushed the seated replicas on his screen and their
// legs glitched as their owners' places pulled them back, and the upright capsules kept everyone
// from walking around the table. While a replica sits (its owner's field "seats" holds a seat),
// and while the local player sits, the replica's physics is switched off (its entity's physics
// component, slot 24 EnablePhysics, as NoClip switches the local body's): the replica is placed by
// its owner's reports anyway (SetWorldTM), and nobody bumps into a seated player. Its physics
// comes back when both stand up, or when the replica goes. The partner of the local player's
// paired animation (playPairedAnimation) is treated as seated while it runs.
//
// Engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

#include <cstdint>

namespace kcds::runtime::seats::collisions {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* physics_component = nullptr;  // the entity's physics component
  void* (*entity_by_id)(std::uint32_t id) = nullptr;
};

// False when a native is missing; the bodies then keep their physics.
bool install(const Natives& natives, Logger logger);

// Every frame for each replica: whether its owner sits, and whether the local player sits.
void keep(std::uint32_t player, std::uint32_t entity_id, bool replica_seated, bool local_seated);

// The replica of player goes: its physics is left as it is (the entity goes with it).
void forget(std::uint32_t player);

// Leaving the world: every replica is forgotten.
void reset();

// A client script's choice (Player.setCollision): the resource runtime sends
// { op: 'playerCollision', id, collides }, and while it is false that player's body on this machine
// passes through as a seated one does. False when the message is not this one. Any thread.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Whether a client script took that player's body here out of the collisions.
bool script_passes_through(std::uint32_t player);

}  // namespace kcds::runtime::seats::collisions
