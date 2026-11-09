// A small server correction of the local player, blended in. When the server's movement check puts
// the player back (a FORCE_XFORM marked as a correction), a short way is not jumped: the offset
// from where the body stands to where the server put it is added over a few frames, on top of the
// player's own movement, so the player keeps walking while the body glides back. A long way, or a
// plain teleport, still jumps at once.
//
// The server's check starts again with the correction's new epoch from the first position the
// owner reports, and judges later steps against kMaxFootSpeed (10 m/s over 0.1 s or more). The
// blend adds at most kBlendSpeed to the player's own speed, so a sprint plus a blend stays under
// that limit. Engine thread; portable, for the unit tests.
#pragma once

#include <algorithm>
#include <cmath>

namespace kcds::runtime::local_player {

// The longest correction blended in, metres; a longer one jumps.
inline constexpr float kBlendMaxMetres = 1.0f;
// Shorter than this is no correction worth a blend (the server's own rounding).
inline constexpr float kBlendMinMetres = 0.02f;
// The most a blend adds to the player's own speed, metres a second.
inline constexpr float kBlendSpeed = 2.5f;

struct CorrectionBlend {
  bool active = false;
  float offset[3] = {};  // the whole way to go
  double started = 0.0;
  double seconds = 0.0;  // how long the blend takes
  float done = 0.0f;     // the share of the offset already added, 0 to 1
};

// Whether a correction `distance` metres long is blended in rather than jumped.
inline bool blends(float distance) {
  return distance >= kBlendMinMetres && distance <= kBlendMaxMetres;
}

// Starts a blend of the offset (to - from) at `now`.
inline void start_blend(CorrectionBlend& blend, const float from[3], const float to[3],
                        double now) {
  float length = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    blend.offset[axis] = to[axis] - from[axis];
    length += blend.offset[axis] * blend.offset[axis];
  }
  blend.active = true;
  blend.started = now;
  // The smooth share's fastest moment is 1.5 times its average speed.
  blend.seconds = 1.5 * std::sqrt(length) / kBlendSpeed;
  blend.done = 0.0f;
}

// The part of the offset to add at `now` (zero once it is all added, which ends the blend). The
// share follows a smooth start and end; each call adds only what the share grew by since the last.
inline void step_blend(CorrectionBlend& blend, double now, float out[3]) {
  out[0] = out[1] = out[2] = 0.0f;
  if (!blend.active) {
    return;
  }
  float share = 1.0f;
  if (blend.seconds > 0.0) {
    const float t = float(std::clamp((now - blend.started) / blend.seconds, 0.0, 1.0));
    share = t * t * (3.0f - 2.0f * t);
  }
  const float grow = share - blend.done;
  for (int axis = 0; axis < 3; ++axis) {
    out[axis] = blend.offset[axis] * grow;
  }
  blend.done = share;
  if (share >= 1.0f) {
    blend.active = false;
  }
}

}  // namespace kcds::runtime::local_player
