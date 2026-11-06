// The level's terrain shaped while the game runs (World Architect terraforming): ground flattened
// under a building, raised or lowered with a brush, every change undoable, solid at once.
//
// WHGame 1.5.5 keeps the terrain's heights per sector node: at node + 192 a block of
// { float base, float step, uint32 samples[size * size], u16 size, u8 lower-detail shift }, a
// sample's height (sample >> 20) * step + base (12 bits of height, the low 20 bits the
// surface). A sample on a sector's first row or column is also the last of the sector before it.
// The game changes its terrain while it runs only for a crater (C3DEngine::OnExplosion
// with e_TerrainDeformations); after a change it rebuilds the terrain nodes in the box
// (on the heightmap's root,per node) and clears its two height caches, and
// this module does the same after it writes the samples (the crater's other step, 3D engine slot
// 68, only deletes the decals in the box). The terrain's collision reads the heights as they are
// now: a player stands on the new ground at once. The heightmap is the terrain's (+ 80): its sector
// grid at + 16 ({ nodes**, width at + 8 }), root node at + 40, sector shift at + 96, size in units
// at + 224.
//
// A sample keeps 4096 heights between its sector's lowest and highest; a height outside them widens
// the sector's range (its samples encoded again, coarser), so a pit can go deep.
//
// The ground's look (Paint, World Architect's texturing): a sample's low 20 bits are up to three of
// its sector's surfaces and their weights (bits 0-3, 4-7 and 8-11 the local surfaces,
// 12-15 and 16-19 the second's and third's weights out of 15, the first takes the rest). A sector
// node maps its 15 local surfaces to the level's surface types through a 16-byte table at + 216
// (127 unused; the load fills it only for the local surfaces its samples use), and keeps
// one detail layer for each surface of its palette in a list at + 168 (32-byte entries whose first
// pointer is the surface type, the level's surface types an array of 440-byte records at the
// terrain's + 200). Paint gives a sample more of a surface the sector's palette has: a free local
// slot is given the surface when it is first used, and the node rebuilt draws it. A World
// Architect custom map's sectors carry 15 ground materials
// (Dev/tools/architect/make_custom_map.py); a game level's sector paints only the surfaces it
// already has. Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::terrain {

using Logger = void (*)(const char* format, ...);

// False when the game's terrain functions were not found (edits are then refused).
bool install(Logger logger);

enum class Shape : std::uint8_t {
  // Every sample in the circle to `value` metres (a flat pad), blended into the ground around.
  Flatten = 1,
  // Every sample in the circle `value` metres up (down when negative), fading at the edge.
  Raise = 2,
  // Every sample in the circle covered with the level's surface type `value` (its index in the
  // level's surface types), blended into the ground's look around over the falloff.
  Paint = 3,
};

struct Edit {
  Shape shape = Shape::Flatten;
  float x = 0.0f;
  float y = 0.0f;
  // Full strength out to `radius` metres, fading to nothing over `falloff` more.
  float radius = 1.0f;
  float falloff = 0.0f;
  float value = 0.0f;
};

struct Result {
  bool applied = false;
  int samples = 0;
  // Samples whose height had to be clamped into their sector's range.
  int clamped = 0;
  // Samples whose sector's range was widened to take them (a deep pit, a high hill).
  int widened = 0;
  // Paint: samples whose sector has not that surface (or no free local surface left).
  int refused = 0;
};

// Shapes the ground; the edit is kept under `id` so revert() can undo it.
Result apply(std::uint32_t id, const Edit& edit);

// Puts back the ground edit `id` changed (later edits over it stay as they are); false when there
// is no such edit.
bool revert(std::uint32_t id);

// The ground's height at the place (its samples, as the game reads them without the blend between
// them); false when there is no terrain there.
bool height_at(float x, float y, float& height);

// The world went: its edits went with it.
void reset();

}  // namespace kcds::runtime::terrain
