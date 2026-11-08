// Decals a client script draws in this game only (World Architect's Decals): the game's own decal,
// as a server marker of shape 'decal' is drawn (world_markers.hpp), without the server.
//
//   { op: 'localDecal', kind: 'show', id, material, x, y, z, size, turn }
//   { op: 'localDecal', kind: 'hide', id }
//
// Their marker ids are kLocalDecalBase + id, apart from the server's markers. Requests: any thread
// (handle_runtime_message). Orders: engine thread (frame).
#pragma once

#include <cstdint>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::markers::local_decals {

inline constexpr std::uint32_t kLocalDecalBase = 0x80000000u;

// True when the message was a localDecal order (it is queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Shows and hides what was ordered since the last frame; in a ready world only.
void frame(bool in_world);

// The world went: the orders not done yet go with it.
void reset();

}  // namespace kcds::runtime::markers::local_decals
