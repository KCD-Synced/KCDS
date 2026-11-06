// World markers the server shows (kcd2s/integration/world_markers.hpp).
//
// A marker is drawn as a decal on the ground with the marker's material, the way the game's own
// Particle.CreateMatDecal script function does it: the game fills a decal description (position,
// surface normal, size, life time, material name, angle) and hands it to the 3D engine's
// CreateDecal (slot 67, C3DEngine::CreateDecal). No script runs.
//
//   - The decal manager has no handle for a decal, so a marker is taken away again with the
//     decal manager's own DeleteDecalsInRange: every decal that covers the marker's
//     position goes, which can include another decal on the same spot.
//   - The look comes from the decal's material. The description has no colour, shape, height,
//     spin or bob, so those fields of the show are not applied; `size` is the decal's radius.
//
// A marker of another shape (cylinder, sphere, chevron, cube, plane) is no decal: it is kept and
// drawn every frame with the renderer's aux geometry (marker_shapes.hpp), in its colour, turned
// by its rotation and its spin (degrees a second about its up axis) and lifted by its bob (metres
// up and down every 2 s); its material is not used.
//
// Engine thread.
#pragma once

#include <cstdint>

#include "kcd2s/integration/world_markers.hpp"

namespace kcds::runtime::markers {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* const* engine_3d = nullptr;  // the slot of the 3D engine
  void* delete_decals_in_range = nullptr;
};

// False when a native is missing; markers are then not drawn.
bool install(const Natives& natives, Logger logger);

// Draws the marker, replacing the one the server showed earlier under the same id.
bool show(const kcd2s::integration::MarkerShow& marker);

// Takes the marker away; true when the client had it.
bool hide(std::uint32_t marker);

// Forgets every marker without touching the engine (the level they were drawn in is gone).
void forget_all();

// Draws the mesh markers this frame; `now` in seconds.
void frame(double now);

}  // namespace kcds::runtime::markers
