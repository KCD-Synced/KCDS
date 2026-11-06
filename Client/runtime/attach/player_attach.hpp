// Players attached to others (Player.attachTo), WHGame 1.5.5.
//
// The attached player's own game puts their body at its offset from the target's every frame and
// lets only looking around and the menus through. The body always turns with the target. Look
// 'follow': the view goes with it, and the looking around goes. Look 'free': free look's camera,
// which the mouse turns about the body, up to 120 degrees either side.
// Every other game shows that body at the same offset from where it shows the target, so the two
// move together on every screen instead of drifting by the network's delay.
//
// An attached body has its physics off on every game, as NoClip does it: pressed into the target,
// the game's actor collision took it for a trample and the target lost health (0.3.4.3 live test,
// DAMAGE reason=4). Engine thread.
#pragma once

#include <cstdint>

#include "kcd2s/integration/player_attach.hpp"

namespace kcds::runtime::attach {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* physics_component = nullptr;  // the entity's physics component
  void* (*local_entity)() = nullptr;  // the local player's entity
  // The entity of another player's body in this game, null when it is not here.
  void* (*replica_entity)(std::uint32_t player) = nullptr;
};

// A body's place: where its feet are and the way it faces (radians, about the up axis).
struct BodyPose {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  float heading = 0.0f;
};

void install(const Natives& natives, Logger logger);

// A server's order; target 0 ends it.
void apply_order(const kcd2s::integration::PlayerAttach& order);

// The client left a world: no orders, and the local body has its physics back.
void clear();

// Every frame, after the replicas have been placed: places the local body and keeps the attached
// replicas without physics.
void frame(bool in_world);

// The order about another player's body, null when it is not attached.
const kcd2s::integration::PlayerAttach* order_of(std::uint32_t player);

// Where the local player's body is now. False when there is none.
bool local_pose(BodyPose& pose);

// The place of a body with this order beside a target at `target`.
BodyPose beside(const kcd2s::integration::PlayerAttach& order, const BodyPose& target);

// Whether the local player is attached with look 'free': the view is free look's camera
// (camera/free_look.hpp), since the body turns with the target.
bool free_look_wanted();

// True when the game must not get this action of the local player (`mode` as the action handler
// gets it).
bool refuse_action(const char* action, unsigned mode);

}  // namespace kcds::runtime::attach
