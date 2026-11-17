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
//
// The target can also be a body by its network id (kAttachToBody): a horse whose passenger sits
// behind its rider (Horse.seatPassenger), placed from where this game shows the horse while it
// climbs on or off, and once seated sitting on a seat of ours that hangs from the horse's rider
// joint (see the seat in player_attach.cpp). A moving
// order (kAttachMoving) carries the body from its `from` offset to its own over its time, as the
// game's mount and dismount clips carry a rider, counted from when this game got the order.
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
  // A body (a horse, a player, an NPC) by its network id, null when this game has none.
  void* (*body_entity)(std::uint32_t network_id) = nullptr;
  // CEntity::AttachChild(parent, child, params): a passenger's seat on the horse's rider joint.
  void* attach_child = nullptr;
  // The body of the rider of a horse by its network id, as this game shows them (the local
  // player's own on their horse); null with none.
  void* (*rider_body)(std::uint32_t horse) = nullptr;
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

// That order with the offset its body has now (a moving order part of the way).
kcd2s::integration::PlayerAttach order_now(std::uint32_t player);

// Where another player's body sits on its seat on a horse (the seat entity's place), so the
// replica is placed exactly there; false when it has no seat (it is placed by its order).
bool seat_pose(std::uint32_t player, BodyPose& pose);

// A horse's entity is about to be removed: the seats on it and the bodies on them let go first,
// so they do not go with it.
void let_go_of(void* entity);

// Where this game has a body by its network id (a horse an order's target is). False when there is
// none.
bool body_pose(std::uint32_t network_id, BodyPose& pose);

// Where the local player's body is now. False when there is none.
bool local_pose(BodyPose& pose);

// The place of a body with this order beside a target at `target`.
BodyPose beside(const kcd2s::integration::PlayerAttach& order, const BodyPose& target);

// Whether the local player is attached to another player or carried on a body (a horse).
bool local_attached();

// The local body while it sits on its seat on a horse (the camera rides on it, camera/free_look.hpp
// ride_on), else null; and the engine's CEntity::AttachChild.
void* seated_body();
void* attach_child_native();

// Whether the local player is attached with look 'free': the view is free look's camera
// (camera/free_look.hpp), since the body turns with the target.
bool free_look_wanted();

// True when the game must not get this action of the local player (`mode` as the action handler
// gets it).
bool refuse_action(const char* action, unsigned mode);

}  // namespace kcds::runtime::attach
