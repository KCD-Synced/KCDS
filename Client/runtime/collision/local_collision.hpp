// Collision shapes a client script adds in this game only (the client Collision,
// Client/resources/collision.cjs; World Architect's Collision layer): invisible solid boxes,
// such as a wall that keeps players off a cliff or a floor over a gap. Nothing is replicated.
//
//   { op: 'localCollision', kind: 'set', id, center, half, rotation }
//   { op: 'localCollision', kind: 'remove', id }
//
// Each box is built as an area's collision wall is (props/placed_props.cpp): the
// game's primitive cube (2 m across, standing on its bottom face) as a static entity, scaled to
// the box's half sizes, given a static body and drawn with the editor material that draws nothing.
// A set of an id that stands builds it again at its new place and size.
//
// Requests: any thread (handle_runtime_message). Orders: engine thread (frame).
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::collision::local {

// True when the message was a localCollision order (it is queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Builds and takes away what was ordered since the last frame; in a ready world only.
void frame(bool in_world);

// The world went (a level change, leaving): its boxes went with it.
void reset();

}  // namespace kcds::runtime::collision::local
