// The props the server places (kcd2s/integration/placed_objects.hpp).
//
// A prop is a BasicEntity showing a static mesh with the order's pose and scale and, by its
// physics, a static or a rigid body or none. The entity is named after the prop's server id, so
// an update (new scale or physics) rebuilds it and a destroy removes it without any bookkeeping
// of the game's entity pointers. The server sends only the props of this player's dimension.
//
// A rigid prop's body is simulated by one game only, the one the server names
// (Simulate): it applies the server's pushes (prop_physics.hpp) and reports where the body is
// while it moves and where it came to rest. Every other game builds the body static and shows it
// where the server's Moves put it.
//
// A prop on a body (Player.attachProp) has no physics and is a child of the body's
// entity, on its joint when the body has one of that name (AttachChild with the joint flag, as the
// lights and bursts on bodies hang): its position and rotation are its offset from there, and a
// Move changes the offset. A body this game does not show (yet) is waited for; a body rebuilt (a
// replica made again) gets the prop hung on it again.
//
// Engine thread.
#pragma once

#include "kcd2s/integration/placed_objects.hpp"

namespace kcds::runtime::props::placed {

using Logger = void (*)(const char* format, ...);
// Sends a report of a body this game simulates to the server; false when it could not.
using ReportFn = bool (*)(const kcd2s::integration::PropReport& report);
// The entity of a player's or NPC's body by its network id, null when this game has none.
using BodyOf = void* (*)(std::uint32_t network_id);

// attach_child: CEntity::AttachChild (props_install.cpp).
void install(Logger logger, ReportFn report, BodyOf body_of, void* attach_child);

// Spawns, rebuilds, moves, pushes or removes a prop.
void apply(const kcd2s::integration::PropOrder& order);

// Each frame (`now` in seconds): the reports of the bodies this game simulates.
void frame(double now);

// The entity showing the server's prop `id` in this game, or null (a World.addInteraction on a
// prop).
void* entity_of(std::uint32_t id);

// Forgets every prop, as the game's level does when it unloads.
void reset();

}  // namespace kcds::runtime::props::placed
