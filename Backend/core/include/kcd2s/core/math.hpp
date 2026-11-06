// Minimal vector/quaternion types for replicated transforms. Game-engine-agnostic; the client
// bridge converts these to/from WHGame types. Kept small on purpose.
#ifndef KCD2S_CORE_MATH_HPP
#define KCD2S_CORE_MATH_HPP

#include <cmath>

namespace kcd2s::core {

struct Vec3 {
  float x = 0.0f, y = 0.0f, z = 0.0f;
  friend bool operator==(const Vec3& a, const Vec3& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
  }
  friend bool operator!=(const Vec3& a, const Vec3& b) {
    return !(a == b);
  }
};

struct Quat {
  float x = 0.0f, y = 0.0f, z = 0.0f, w = 1.0f;
  friend bool operator==(const Quat& a, const Quat& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
  }
  friend bool operator!=(const Quat& a, const Quat& b) {
    return !(a == b);
  }
};

inline float distance_sq(const Vec3& a, const Vec3& b) {
  const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}
inline float distance(const Vec3& a, const Vec3& b) {
  return std::sqrt(distance_sq(a, b));
}

}  // namespace kcd2s::core

#endif  // KCD2S_CORE_MATH_HPP
