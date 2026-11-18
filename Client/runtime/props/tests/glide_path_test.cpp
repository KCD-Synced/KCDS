#include "../glide_path.hpp"

#include <cmath>

#include "kcd2s/testkit.hpp"

using kcd2s::integration::PropEase;
using kcd2s::integration::PropPathKey;
using kcds::runtime::props::glide::Glide;
using kcds::runtime::props::glide::pose_at;
using kcds::runtime::props::glide::spin_turn;

namespace {

bool near(float a, float b, float within = 1e-4f) {
  return std::fabs(a - b) <= within;
}

// A glide from the origin to `to` over `ms`, lifted `arc` metres at its middle.
Glide straight(PropEase ease, float to_x, std::uint32_t ms, float arc) {
  Glide glide;
  glide.ease = ease;
  PropPathKey end;
  end.ms = ms;
  end.position = {to_x, 0.0f, 0.0f};
  end.arc = arc;
  glide.keys.push_back(end);
  return glide;
}

}  // namespace

// The eased glide is the one Prop.glideTo always had: slow at both ends, a sine arc.
KCD2S_TEST(glide_path, smooth_glide_eases_in_and_out) {
  const Glide glide = straight(PropEase::Smooth, 10.0f, 1000, 1.0f);
  float at[3]{};
  float turn[4]{};
  KCD2S_CHECK(pose_at(glide, 0.25, at, turn));
  KCD2S_CHECK(near(at[0], 10.0f * 0.15625f));
  KCD2S_CHECK(near(at[2], float(std::sin(3.14159265358979 * 0.25))));
  KCD2S_CHECK(pose_at(glide, 0.5, at, turn));
  KCD2S_CHECK(near(at[0], 5.0f));
  KCD2S_CHECK(near(at[2], 1.0f));
}

// The linear glide goes at an even pace on a parabola: a thrown body's flight.
KCD2S_TEST(glide_path, linear_glide_flies_a_parabola) {
  const Glide glide = straight(PropEase::Linear, 10.0f, 1000, 1.0f);
  float at[3]{};
  float turn[4]{};
  KCD2S_CHECK(pose_at(glide, 0.25, at, turn));
  KCD2S_CHECK(near(at[0], 2.5f));
  KCD2S_CHECK(near(at[2], 0.75f));
  KCD2S_CHECK(pose_at(glide, 0.5, at, turn));
  KCD2S_CHECK(near(at[2], 1.0f));
  // The end holds at the key; the glide no longer runs.
  KCD2S_CHECK(!pose_at(glide, 1.5, at, turn));
  KCD2S_CHECK(near(at[0], 10.0f));
  KCD2S_CHECK(near(at[2], 0.0f));
}

// A path goes key to key without stopping: each way starts where the key before it is.
KCD2S_TEST(glide_path, path_goes_key_to_key) {
  Glide glide = straight(PropEase::Linear, 4.0f, 400, 0.5f);
  PropPathKey bounce;
  bounce.ms = 600;
  bounce.position = {5.0f, 0.0f, 0.0f};
  bounce.arc = 0.1f;
  glide.keys.push_back(bounce);
  float at[3]{};
  float turn[4]{};
  KCD2S_CHECK(pose_at(glide, 0.4, at, turn));
  KCD2S_CHECK(near(at[0], 4.0f));
  KCD2S_CHECK(near(at[2], 0.0f));
  KCD2S_CHECK(pose_at(glide, 0.5, at, turn));
  KCD2S_CHECK(near(at[0], 4.5f));
  KCD2S_CHECK(near(at[2], 0.1f));
  KCD2S_CHECK(!pose_at(glide, 0.6, at, turn));
  KCD2S_CHECK(near(at[0], 5.0f));
}

// A spin turns it about the world's axes on top of the keys' turns.
KCD2S_TEST(glide_path, spin_turns_on_top) {
  Glide glide = straight(PropEase::Linear, 1.0f, 1000, 0.0f);
  glide.spin[2] = 3.14159265f;
  float at[3]{};
  float turn[4]{};
  KCD2S_CHECK(pose_at(glide, 0.5, at, turn));
  // Half a second at pi a second: a quarter turn about z.
  KCD2S_CHECK(near(turn[2], float(std::sin(3.14159265 / 4.0))));
  KCD2S_CHECK(near(turn[3], float(std::cos(3.14159265 / 4.0))));
  float spun[4]{};
  spin_turn(glide.spin, 1.0, spun);
  KCD2S_CHECK(!pose_at(glide, 1.0, at, turn));
  KCD2S_CHECK(near(turn[2], spun[2]));
  KCD2S_CHECK(near(turn[3], spun[3]));
}
