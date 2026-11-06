#include "paired_start.hpp"

#include <cmath>
#include <cstddef>

namespace kcds::runtime::animation::paired_start {

namespace {

// IAnimationSet::GetAnimationDCCWorldSpaceLocation(int id, QuatT& location).
constexpr std::size_t kStartLocationSlot = 20;
static_assert(sizeof(Place) == 28, "a Place is laid out as the engine's QuatT");
using StartLocation = bool(__fastcall*)(void* set, int id, Place* place);

// q * v * conjugate(q).
void rotate(const float q[4], const float v[3], float out[3]) {
  const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
  const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
  const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
  out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
  out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
  out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

}  // namespace

Place compose(const Place& a, const Place& b) {
  Place out;
  out.q[0] = a.q[3] * b.q[0] + a.q[0] * b.q[3] + a.q[1] * b.q[2] - a.q[2] * b.q[1];
  out.q[1] = a.q[3] * b.q[1] - a.q[0] * b.q[2] + a.q[1] * b.q[3] + a.q[2] * b.q[0];
  out.q[2] = a.q[3] * b.q[2] + a.q[0] * b.q[1] - a.q[1] * b.q[0] + a.q[2] * b.q[3];
  out.q[3] = a.q[3] * b.q[3] - a.q[0] * b.q[0] - a.q[1] * b.q[1] - a.q[2] * b.q[2];
  float moved[3];
  rotate(a.q, b.t, moved);
  for (int axis = 0; axis < 3; ++axis) {
    out.t[axis] = a.t[axis] + moved[axis];
  }
  return out;
}

Place inverse(const Place& a) {
  Place out;
  out.q[0] = -a.q[0];
  out.q[1] = -a.q[1];
  out.q[2] = -a.q[2];
  out.q[3] = a.q[3];
  float back[3];
  rotate(out.q, a.t, back);
  for (int axis = 0; axis < 3; ++axis) {
    out.t[axis] = -back[axis];
  }
  return out;
}

Place heading_only(const Place& a) {
  // The heading of the turned forward axis (y), as the engine's yaw.
  const float forward[3] = {0.0f, 1.0f, 0.0f};
  float turned[3];
  rotate(a.q, forward, turned);
  const float yaw = std::atan2(-turned[0], turned[1]);
  Place out;
  out.q[2] = std::sin(yaw * 0.5f);
  out.q[3] = std::cos(yaw * 0.5f);
  for (int axis = 0; axis < 3; ++axis) {
    out.t[axis] = a.t[axis];
  }
  return out;
}

Place slave_place(const Place& master, const Place& master_start, const Place& slave_start) {
  return heading_only(compose(compose(master, inverse(master_start)), slave_start));
}

bool start_location(void* animation_set, int clip, Place& place) {
  if (!animation_set || clip < 0) {
    return false;
  }
  auto table = *static_cast<void***>(animation_set);
  return reinterpret_cast<StartLocation>(table[kStartLocationSlot])(animation_set, clip, &place);
}

}  // namespace kcds::runtime::animation::paired_start
