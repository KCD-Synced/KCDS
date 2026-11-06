// Trees, bushes and the like a client script plants in this game only (the client Vegetation,
// Client/resources/vegetation.cjs; World Architect): any kind of vegetation the level has, planted
// as the level's own are, solid, bending in the wind.
//
// WHGame 1.5.5 keeps the level's vegetation kinds as groups: a vector of 416-byte entries at
// * + 16 (+ 0 the kind's mesh, + 408 the list of its instances). The level loader lays
// an instance: a CVegetation (3D engine CreateRenderNode, slot 156, type 2) with its
// place at + 80, its group at + 120, its turn (a byte of 256ths of a circle) at + 124, its scale
// times 64 at + 125, its box around the place as bytes at + 126 (x, y, z up) and + 129 (x, y, z
// down), each the extent in metres * 3.984375 + 1, and its radius at + 132 likewise; then
// physicalizes it (at once with true), adds it to its group's list,
// registers it (3D engine slot 38) and readies its bending. This module does the same.
// A planted instance is taken away as LevelEdit hides one (render node slot 68 Hide, slot 48
// Dephysicalize).
//
// Client scripts ask through the sync channel ({ op: 'syncRequest', id, request }):
//   { op: 'localVegetationKinds' }  -> { kinds: [mesh path, ...] } (a group's index is its kind)
//   { op: 'localVegetationPlant', kind, x, y, z, scale, turn } -> { id } or null
//   { op: 'localVegetationRemove', id } -> true or false
// answered on the engine thread (answer) and sent by the network thread (pump).
#pragma once

#include <functional>
#include <string>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::vegetation {

using Logger = void (*)(const char* format, ...);

// False when the game's vegetation functions were not found (requests are then answered null).
bool install(Logger logger);

// True when the message was a vegetation request (it is queued).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Answers the requests queued since the last frame; engine thread.
void answer(bool in_world);

// Sends the answers; network thread.
void pump(const std::function<bool(const kcd2s::sdk::Value&)>& send);

// A vegetation kind's mesh path (lower case, forward slashes); "" when the level has no such kind.
// Engine thread.
std::string kind_path(int kind);

// The world went: the planted vegetation went with it.
void reset();

}  // namespace kcds::runtime::vegetation
