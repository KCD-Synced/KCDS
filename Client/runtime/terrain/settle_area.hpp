// The area where edits made together settle at once (terrain_edits.cpp): the game's terrain nodes
// rebuilt, its height caches cleared, the roads laid again and the grass moved once over all of
// them, instead of once for every edit. Boxes are { min x, min y, min z, max x, max y, max z }.
//
// Portable: the unit test builds it.
#pragma once

#include <algorithm>

namespace kcds::runtime::terrain {

struct SettleArea {
  bool any = false;
  float box[6] = {};
  int edits = 0;
};

// Adds an edit's box to the area. False (the area unchanged) when the area would then be wider or
// deeper than `most` metres: the caller settles the area first and starts a new one with the box.
inline bool join(SettleArea& area, const float box[6], float most) {
  if (!area.any) {
    std::copy(box, box + 6, area.box);
    area.any = true;
    area.edits = 1;
    return true;
  }
  float joined[6];
  for (int axis = 0; axis < 3; ++axis) {
    joined[axis] = (std::min)(area.box[axis], box[axis]);
    joined[axis + 3] = (std::max)(area.box[axis + 3], box[axis + 3]);
  }
  if (joined[3] - joined[0] > most || joined[4] - joined[1] > most) {
    return false;
  }
  std::copy(joined, joined + 6, area.box);
  ++area.edits;
  return true;
}

}  // namespace kcds::runtime::terrain
