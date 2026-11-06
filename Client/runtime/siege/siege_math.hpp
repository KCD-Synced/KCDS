// The vector and quaternion arithmetic of the siege ladders and stone piles.
// Quaternions are (x, y, z, w), as the server sends them and the game's entities take them.
#pragma once

#include <cmath>
#include <cstddef>

namespace kcds::runtime::siege::math {

inline constexpr float kRadiansPerDegree = 3.14159265358979f / 180.0f;

// The rotation of `radians` about the x axis.
inline void axis_x(float radians, float out[4]) {
  out[0] = std::sin(radians * 0.5f);
  out[1] = 0.0f;
  out[2] = 0.0f;
  out[3] = std::cos(radians * 0.5f);
}

// a then b in a's frame: the product a * b.
inline void multiply(const float a[4], const float b[4], float out[4]) {
  const float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  const float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  const float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  const float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  out[0] = x;
  out[1] = y;
  out[2] = z;
  out[3] = w;
}

// The vector turned by the rotation.
inline void rotate(const float q[4], const float v[3], float out[3]) {
  // t = 2 * (q.xyz x v); v' = v + w * t + q.xyz x t
  const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
  const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
  const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
  out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
  out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
  out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

inline float distance(const float a[3], const float b[3]) {
  const float dx = a[0] - b[0];
  const float dy = a[1] - b[1];
  const float dz = a[2] - b[2];
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace kcds::runtime::siege::math
