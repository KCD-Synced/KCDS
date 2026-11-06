// Small quaternion helpers for the previews and gizmos (CryEngine's x, y, z, w order, z up).
#pragma once

#include <cmath>

namespace kcds::runtime::pose_math {

// q * v * q^-1 for a unit quaternion.
inline void rotate(const float q[4], const float v[3], float out[3]) {
  const float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
  const float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
  const float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);
  out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
  out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
  out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

// a * b: b first, then a.
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

// A turn of `radians` about the vertical.
inline void about_z(float radians, float out[4]) {
  out[0] = 0.0f;
  out[1] = 0.0f;
  out[2] = std::sin(radians * 0.5f);
  out[3] = std::cos(radians * 0.5f);
}

// The shortest turn that brings straight up onto the unit vector `to`.
inline void up_onto(const float to[3], float out[4]) {
  const float dot = to[2];
  if (dot < -0.9999f) {
    out[0] = 1.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = 0.0f;
    return;
  }
  // up x to = (-to.y, to.x, 0)
  float x = -to[1];
  float y = to[0];
  float z = 0.0f;
  float w = 1.0f + dot;
  const float length = std::sqrt(x * x + y * y + z * z + w * w);
  out[0] = x / length;
  out[1] = y / length;
  out[2] = z / length;
  out[3] = w / length;
}

// The inverse turn of a unit quaternion.
inline void conjugate(const float q[4], float out[4]) {
  out[0] = -q[0];
  out[1] = -q[1];
  out[2] = -q[2];
  out[3] = q[3];
}

// A 3x4 row-major world matrix (CryEngine's Matrix34, its columns the axes) of a position, a unit
// quaternion turn and a uniform scale.
inline void to_matrix(const float position[3], const float q[4], float scale, float out[12]) {
  const float x = q[0];
  const float y = q[1];
  const float z = q[2];
  const float w = q[3];
  out[0] = (1.0f - 2.0f * (y * y + z * z)) * scale;
  out[1] = 2.0f * (x * y - w * z) * scale;
  out[2] = 2.0f * (x * z + w * y) * scale;
  out[3] = position[0];
  out[4] = 2.0f * (x * y + w * z) * scale;
  out[5] = (1.0f - 2.0f * (x * x + z * z)) * scale;
  out[6] = 2.0f * (y * z - w * x) * scale;
  out[7] = position[1];
  out[8] = 2.0f * (x * z - w * y) * scale;
  out[9] = 2.0f * (y * z + w * x) * scale;
  out[10] = (1.0f - 2.0f * (x * x + y * y)) * scale;
  out[11] = position[2];
}

// The turn of a 3x4 row-major world matrix (CryEngine's Matrix34, its columns the axes), its
// scale taken out.
inline void from_matrix(const float m[12], float out[4]) {
  float axes[3][3];
  for (int column = 0; column < 3; ++column) {
    const float x = m[column];
    const float y = m[4 + column];
    const float z = m[8 + column];
    float length = std::sqrt(x * x + y * y + z * z);
    if (!(length > 1e-6f)) {
      length = 1.0f;
    }
    axes[0][column] = x / length;
    axes[1][column] = y / length;
    axes[2][column] = z / length;
  }
  const float trace = axes[0][0] + axes[1][1] + axes[2][2];
  if (trace > 0.0f) {
    const float s = std::sqrt(trace + 1.0f) * 2.0f;
    out[3] = 0.25f * s;
    out[0] = (axes[2][1] - axes[1][2]) / s;
    out[1] = (axes[0][2] - axes[2][0]) / s;
    out[2] = (axes[1][0] - axes[0][1]) / s;
  } else if (axes[0][0] > axes[1][1] && axes[0][0] > axes[2][2]) {
    const float s = std::sqrt(1.0f + axes[0][0] - axes[1][1] - axes[2][2]) * 2.0f;
    out[3] = (axes[2][1] - axes[1][2]) / s;
    out[0] = 0.25f * s;
    out[1] = (axes[0][1] + axes[1][0]) / s;
    out[2] = (axes[0][2] + axes[2][0]) / s;
  } else if (axes[1][1] > axes[2][2]) {
    const float s = std::sqrt(1.0f + axes[1][1] - axes[0][0] - axes[2][2]) * 2.0f;
    out[3] = (axes[0][2] - axes[2][0]) / s;
    out[0] = (axes[0][1] + axes[1][0]) / s;
    out[1] = 0.25f * s;
    out[2] = (axes[1][2] + axes[2][1]) / s;
  } else {
    const float s = std::sqrt(1.0f + axes[2][2] - axes[0][0] - axes[1][1]) * 2.0f;
    out[3] = (axes[1][0] - axes[0][1]) / s;
    out[0] = (axes[0][2] + axes[2][0]) / s;
    out[1] = (axes[1][2] + axes[2][1]) / s;
    out[2] = 0.25f * s;
  }
}

// The pose (position, turn) of `child` seen from `parent`, both in the world.
inline void relative(const float parent_position[3], const float parent_rotation[4],
                     const float child_position[3], const float child_rotation[4],
                     float position[3], float rotation[4]) {
  float inverse[4];
  conjugate(parent_rotation, inverse);
  const float offset[3] = {child_position[0] - parent_position[0],
                           child_position[1] - parent_position[1],
                           child_position[2] - parent_position[2]};
  rotate(inverse, offset, position);
  multiply(inverse, child_rotation, rotation);
}

// The world pose of something at `position`, `rotation` relative to `parent`.
inline void compose(const float parent_position[3], const float parent_rotation[4],
                    const float position[3], const float rotation[4], float world_position[3],
                    float world_rotation[4]) {
  float turned[3];
  rotate(parent_rotation, position, turned);
  for (int axis = 0; axis < 3; ++axis) {
    world_position[axis] = parent_position[axis] + turned[axis];
  }
  multiply(parent_rotation, rotation, world_rotation);
}

inline void normalize(float q[4]) {
  const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(length > 1e-6f) || !std::isfinite(length)) {
    q[0] = 0.0f;
    q[1] = 0.0f;
    q[2] = 0.0f;
    q[3] = 1.0f;
    return;
  }
  for (int i = 0; i < 4; ++i) {
    q[i] /= length;
  }
}

}  // namespace kcds::runtime::pose_math
