// Positions and rotations as the level's XML writes them ("x,y,z"; rotations "w,x,y,z"), and
// placing an object's parts in the world. Plain code, no game calls.
#pragma once

#include <string>

namespace kcds::runtime::kcd1 {

struct Vec3 {
  double x = 0;
  double y = 0;
  double z = 0;
};

// A rotation: w, x, y, z.
struct Quat {
  double w = 1;
  double x = 0;
  double y = 0;
  double z = 0;
};

// "x,y,z" (false when it is not three numbers) and "w,x,y,z" (the identity when it is not four).
bool read_position(const std::string& text, Vec3& at);
Quat read_rotation(const std::string& text);

std::string written(const Vec3& at);
std::string written(const Quat& q);

// `a` followed by `b` (b turned by a).
Quat combined(const Quat& a, const Quat& b);

// `at` turned by `q`.
Vec3 turned(const Quat& q, const Vec3& at);

// Where a part at `local` (position and rotation in its object's axes) stands in the world when
// the object stands at `position` turned by `rotation`.
Vec3 placed(const Vec3& position, const Quat& rotation, const Vec3& local);

}  // namespace kcds::runtime::kcd1
