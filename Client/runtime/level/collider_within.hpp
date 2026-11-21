// Which of the level's invisible colliders go with an object the author deleted or moved away.
//
// The level gives many objects their collision as a separate invisible collider (a static body
// with neither a render node nor an entity): a yard gate's, a fence's, a pile's. It stands within
// the object's bounds but seldom at the object's own place, so a deleted gate left a wall people
// walked into. A collider is the object's when its bounds lie within the object's (a little
// margin around them), and it stands up from the ground: a flat one at the object's feet is a floor
// the object stood on (the floor laid over a castle gateway's hole), which stays.
//
// Portable: the unit test builds it.
#pragma once

namespace kcds::runtime::level {

// How far a collider may reach past the object's bounds, metres.
inline constexpr float kColliderMargin = 0.3f;
// A collider lower than this, or whose top is not this far above the object's bottom, is a floor.
inline constexpr float kColliderStandsUp = 0.5f;

// collider: min x y z, max x y z; object: its bounds where the level placed it.
inline bool collider_within(const float collider[6], const float object_min[3],
                            const float object_max[3]) {
  for (int axis = 0; axis < 3; ++axis) {
    if (collider[axis] < object_min[axis] - kColliderMargin ||
        collider[axis + 3] > object_max[axis] + kColliderMargin) {
      return false;
    }
  }
  const float height = collider[5] - collider[2];
  const float top_above_bottom = collider[5] - object_min[2];
  return height >= kColliderStandsUp && top_above_bottom >= kColliderStandsUp;
}

}  // namespace kcds::runtime::level
