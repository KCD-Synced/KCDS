// Where a gliding prop is at a moment of its glide (Prop.glideTo, Prop.followPath, Prop.launch, and
// a rigid body another game simulates followed from pose to pose). Every game plays the same glide
// each frame from the one order the server sent.
#pragma once

#include <vector>

#include "kcd2s/integration/placed_objects.hpp"

namespace kcds::runtime::props::glide {

struct Glide {
  // Where it stood when the glide began.
  float from[3]{};
  float from_turn[4]{0.0f, 0.0f, 0.0f, 1.0f};
  // The poses it passes in time order, the last where it ends.
  std::vector<kcd2s::integration::PropPathKey> keys;
  kcd2s::integration::PropEase ease = kcd2s::integration::PropEase::Smooth;
  // Radians a second about the world's axes, turned on top of the keys' rotations.
  float spin[3]{};
};

// The pose `seconds` after the glide began (held at the last key's pose, spun, once it ended);
// true while the glide still runs.
bool pose_at(const Glide& glide, double seconds, float at[3], float turn[4]);

// The turn `spin` (radians a second about the world's axes) makes in `seconds`.
void spin_turn(const float spin[3], double seconds, float out[4]);

}  // namespace kcds::runtime::props::glide
