// The local player's prop previews (GHOST in kcd2s/integration/previews.hpp).
//
// A preview is one BasicEntity per mesh, without physics, named after the preview and the piece,
// posed from the preview's position, rotation and scale and each piece's offset and rotation. It
// is drawn translucent with the entity's opacity (prop_entity::set_opacity): the tint's alpha, or
// kGhostOpacity without a tint. The tint's colour is not laid on the meshes' materials (they are
// shared with every other copy of the mesh); a ring of that colour is drawn on the ground under the
// preview each frame instead (gizmos::circle). A hidden preview has no entities.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/previews.hpp"

namespace kcds::runtime::props::ghosts {

using Logger = void (*)(const char* format, ...);

inline constexpr float kGhostOpacity = 0.5f;

// One preview: what it shows and where; the placer (placer.hpp) has one too.
struct Preview {
  std::string name;  // entity names are name + "_" + piece index
  std::vector<kcd2s::integration::PreviewPiece> pieces;
  float position[3]{};
  float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f};
  float scale = 1.0f;
  // Per axis on top of `scale` (PropGhost scale as a Vector3).
  float axes[3]{1.0f, 1.0f, 1.0f};
  kcd2s::integration::PreviewTint tint;
  // Drawn in kSeeThroughMaterial (World Architect's previews): the entity opacity does not reach
  // the drawing of these meshes (the render proxy's SetOpacity, only keeps a byte).
  bool see_through = false;
  bool shown = false;  // its entities stand
};

// Spawns the preview's entities (again) at its pose; false when no mesh could be shown.
bool show(Preview& preview);

// Moves the standing entities to the preview's pose.
void place(const Preview& preview);

// Sets the standing entities' opacity from the preview's tint.
void fade(const Preview& preview);

// Removes the preview's entities.
void hide(Preview& preview);

// The tint ring under a shown, tinted preview, for this frame.
void draw_ring(const Preview& preview);

void install(Logger logger);

// Creates, moves, tints, hides or removes one of the server's previews.
void apply(const kcd2s::integration::GhostOrder& order);

// Draws the tint rings of this frame.
void frame();

// A ghost's box in the world changed (all its pieces together), or it has none (hidden, its meshes
// not loaded): for the client scripts' own ghosts (local_previews.hpp, PropGhost.getBounds).
struct BoundsChange {
  std::uint32_t ghost = 0;
  bool present = false;
  float min[3]{};
  float max[3]{};
};
// The changes since the last call, for the ghosts with kLocalBit (engine thread).
std::vector<BoundsChange> take_local_bounds();

// Forgets every preview, as the game's level does when it unloads (the server sends them again).
void reset();

}  // namespace kcds::runtime::props::ghosts
