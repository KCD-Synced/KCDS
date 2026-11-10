// Two-bone reach for an arm (shoulder, elbow, hand), and the small rotation helpers it needs.
// Quaternions are (x, y, z, w); rotations act on vectors as q * v * q^-1.
//
// A player woman plays the male clips on her own skeleton (female_retarget.hpp): every joint turns
// as the male joint does, but her shoulders are narrower and her arms shorter, so her hands do not
// end where a man's do. Drawing a bow, her hand stayed behind her ear while the arrow, made for a
// man's draw, ended in her face. bend_change and rotation_between let her arm reach where the man's
// hand is: the elbow bends to the target's distance, then the shoulder turns the hand onto it.
#pragma once

#include <algorithm>
#include <cmath>

namespace kcds::runtime::animation::arm_ik {

inline float dot(const float a[3], const float b[3]) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

inline void cross(const float a[3], const float b[3], float out[3]) {
  const float x = a[1] * b[2] - a[2] * b[1];
  const float y = a[2] * b[0] - a[0] * b[2];
  const float z = a[0] * b[1] - a[1] * b[0];
  out[0] = x;
  out[1] = y;
  out[2] = z;
}

inline float length(const float v[3]) {
  return std::sqrt(dot(v, v));
}

inline void subtract(const float a[3], const float b[3], float out[3]) {
  out[0] = a[0] - b[0];
  out[1] = a[1] - b[1];
  out[2] = a[2] - b[2];
}

// The rotation by `angle` radians around the unit `axis`.
inline void axis_angle(const float axis[3], float angle, float q[4]) {
  const float half = 0.5f * angle;
  const float s = std::sin(half);
  q[0] = axis[0] * s;
  q[1] = axis[1] * s;
  q[2] = axis[2] * s;
  q[3] = std::cos(half);
}

inline void rotate(const float q[4], const float v[3], float out[3]) {
  const float u[3] = {2.0f * (q[1] * v[2] - q[2] * v[1]), 2.0f * (q[2] * v[0] - q[0] * v[2]),
                      2.0f * (q[0] * v[1] - q[1] * v[0])};
  out[0] = v[0] + q[3] * u[0] + (q[1] * u[2] - q[2] * u[1]);
  out[1] = v[1] + q[3] * u[1] + (q[2] * u[0] - q[0] * u[2]);
  out[2] = v[2] + q[3] * u[2] + (q[0] * u[1] - q[1] * u[0]);
}

// The shortest rotation taking the direction of `from` onto the direction of `to`; none when either
// is too short to have a direction.
inline void rotation_between(const float from[3], const float to[3], float q[4]) {
  q[0] = q[1] = q[2] = 0.0f;
  q[3] = 1.0f;
  const float a = length(from);
  const float b = length(to);
  if (a < 1e-5f || b < 1e-5f) {
    return;
  }
  float axis[3];
  cross(from, to, axis);
  const float sine = length(axis) / (a * b);
  const float cosine = dot(from, to) / (a * b);
  if (sine < 1e-6f) {
    return;  // Already along it (or exactly opposite, which an arm's small corrections never are).
  }
  axis[0] /= sine * a * b;
  axis[1] /= sine * a * b;
  axis[2] /= sine * a * b;
  axis_angle(axis, std::atan2(sine, cosine), q);
}

// How far the elbow must bend, around `axis_out` (unit, through the elbow), for the hand to be the
// target's distance from the shoulder: the angle to turn the forearm by, positive opening the arm.
// The distance is kept a little inside what the two bones can span. Zero when the arm is too
// short or straight in a line with no plane to bend in.
inline float bend_change(const float shoulder[3], const float elbow[3], const float hand[3],
                         const float target[3], float axis_out[3]) {
  float upper[3];
  float lower[3];
  float reach[3];
  subtract(elbow, shoulder, upper);
  subtract(hand, elbow, lower);
  subtract(target, shoulder, reach);
  const float a = length(upper);
  const float b = length(lower);
  if (a < 1e-4f || b < 1e-4f) {
    return 0.0f;
  }
  cross(upper, lower, axis_out);
  const float axis_length = length(axis_out);
  if (axis_length < 1e-6f) {
    return 0.0f;
  }
  axis_out[0] /= axis_length;
  axis_out[1] /= axis_length;
  axis_out[2] /= axis_length;
  const float span = 0.999f * (a + b);
  const float least = std::abs(a - b) + 0.001f * (a + b);
  const float d = std::clamp(length(reach), least, span);
  // The angle between the upper arm's direction and the forearm's (0 straight).
  const float now = std::acos(std::clamp(dot(upper, lower) / (a * b), -1.0f, 1.0f));
  const float wanted = std::acos(std::clamp((d * d - a * a - b * b) / (2.0f * a * b), -1.0f, 1.0f));
  return wanted - now;
}

// Between rotations `a` (t = 0) and `b` (t = 1), on the short way round.
inline void blend(const float a[4], const float b[4], float t, float out[4]) {
  const float sign = (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]) < 0.0f ? -1.0f : 1.0f;
  float n = 0.0f;
  for (int i = 0; i < 4; ++i) {
    out[i] = a[i] + (sign * b[i] - a[i]) * t;
    n += out[i] * out[i];
  }
  n = std::sqrt(n);
  if (n < 1e-6f) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    return;
  }
  for (int i = 0; i < 4; ++i) {
    out[i] /= n;
  }
}

}  // namespace kcds::runtime::animation::arm_ik
