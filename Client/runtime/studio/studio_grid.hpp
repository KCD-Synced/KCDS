// The studio's grid and axes in the view: a grid of one-metre squares on the floor
// round the character's stage, 20 m across, with the world's x axis (red) and y axis (green)
// through the stage drawn over it, clipped to the editor's view (studio_view). The toolbar's
// Grid switch turns it on and off.
//
//   { op: 'studioGrid', on }
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::grid {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

void tick();

}  // namespace kcds::runtime::studio::grid
