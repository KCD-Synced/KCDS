#include "glide_path.hpp"

#include <algorithm>
#include <cmath>

#include "pose_math.hpp"

namespace kcds::runtime::props::glide {
namespace {

using kcd2s::integration::PropEase;
using kcd2s::integration::PropPathKey;

constexpr double kPi = 3.14159265358979;

// The turn between `a` and `b` at `share` of the way, the shorter way round.
void blend_turn(const float a[4], const float b[4], float share, float out[4]) {
  float dot = 0.0f;
  for (int i = 0; i < 4; ++i) {
    dot += a[i] * b[i];
  }
  const float sign = dot < 0.0f ? -1.0f : 1.0f;
  for (int i = 0; i < 4; ++i) {
    out[i] = a[i] + (sign * b[i] - a[i]) * share;
  }
  pose_math::normalize(out);
}

}  // namespace

void spin_turn(const float spin[3], double seconds, float out[4]) {
  const double rate = std::sqrt(spin[0] * spin[0] + spin[1] * spin[1] + spin[2] * spin[2]);
  if (rate <= 0.0) {
    out[0] = 0.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 1.0f;
    return;
  }
  const double half = rate * seconds / 2.0;
  const double s = std::sin(half) / rate;
  out[0] = float(spin[0] * s);
  out[1] = float(spin[1] * s);
  out[2] = float(spin[2] * s);
  out[3] = float(std::cos(half));
}

bool pose_at(const Glide& glide, double seconds, float at[3], float turn[4]) {
  if (glide.keys.empty()) {
    for (int i = 0; i < 3; ++i) {
      at[i] = glide.from[i];
    }
    for (int i = 0; i < 4; ++i) {
      turn[i] = glide.from_turn[i];
    }
    return false;
  }
  const double total = glide.keys.back().ms / 1000.0;
  const double now = std::clamp(seconds, 0.0, total);
  // The way it is on: from the pose before it (where it began, for the first) to its key.
  std::size_t index = 0;
  while (index + 1 < glide.keys.size() && now > glide.keys[index].ms / 1000.0) {
    ++index;
  }
  const PropPathKey& key = glide.keys[index];
  const double begins = index == 0 ? 0.0 : glide.keys[index - 1].ms / 1000.0;
  const float* start = index == 0 ? glide.from : glide.keys[index - 1].position.data();
  const float* start_turn = index == 0 ? glide.from_turn : glide.keys[index - 1].rotation.data();
  const double length = key.ms / 1000.0 - begins;
  const double t = length > 0.0 ? std::clamp((now - begins) / length, 0.0, 1.0) : 1.0;
  // Linear: an even pace and a parabola, the way a thrown body flies between two places. Smooth:
  // eased in and out under a sine arc.
  const bool linear = glide.ease == PropEase::Linear;
  const float share = linear ? float(t) : float(t * t * (3.0 - 2.0 * t));
  const float lift = linear ? float(4.0 * t * (1.0 - t)) : float(std::sin(kPi * t));
  for (int i = 0; i < 3; ++i) {
    at[i] = start[i] + (key.position[i] - start[i]) * share;
  }
  at[2] += key.arc * lift;
  float base[4]{};
  blend_turn(start_turn, key.rotation.data(), share, base);
  float spun[4]{};
  spin_turn(glide.spin, now, spun);
  pose_math::multiply(spun, base, turn);
  pose_math::normalize(turn);
  return seconds < total;
}

}  // namespace kcds::runtime::props::glide
