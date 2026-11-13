#include "placement.hpp"

#include <cstdio>

namespace kcds::runtime::kcd1 {

bool read_position(const std::string& text, Vec3& at) {
  return std::sscanf(text.c_str(), "%lf,%lf,%lf", &at.x, &at.y, &at.z) == 3;
}

Quat read_rotation(const std::string& text) {
  Quat q;
  if (std::sscanf(text.c_str(), "%lf,%lf,%lf,%lf", &q.w, &q.x, &q.y, &q.z) != 4) {
    return Quat{};
  }
  return q;
}

std::string written(const Vec3& at) {
  char out[96] = {};
  std::snprintf(out, sizeof(out), "%.7g,%.7g,%.7g", at.x, at.y, at.z);
  return out;
}

std::string written(const Quat& q) {
  char out[128] = {};
  std::snprintf(out, sizeof(out), "%.7g,%.7g,%.7g,%.7g", q.w, q.x, q.y, q.z);
  return out;
}

Quat combined(const Quat& a, const Quat& b) {
  return {
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

Vec3 turned(const Quat& q, const Vec3& at) {
  // q * (0, at) * conjugate(q)
  const Quat point{0, at.x, at.y, at.z};
  const Quat conjugate{q.w, -q.x, -q.y, -q.z};
  const Quat out = combined(combined(q, point), conjugate);
  return {out.x, out.y, out.z};
}

Vec3 placed(const Vec3& position, const Quat& rotation, const Vec3& local) {
  const Vec3 offset = turned(rotation, local);
  return {position.x + offset.x, position.y + offset.y, position.z + offset.z};
}

}  // namespace kcds::runtime::kcd1
