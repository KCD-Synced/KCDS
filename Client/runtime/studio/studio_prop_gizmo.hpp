// The studio's gizmo for props (Asset/Animation Studio): the rope or whip (where it
// is held, studio_hold) and your mesh (studio_mesh) are picked with a press in the view and moved,
// turned and sized by dragging the gizmo's handles, as in World Architect.
//
//   { op: 'studioPropGizmo', thing: 'prop' | 'end' | 'mesh' | '',
//     tool: 'move' | 'rotate' | 'scale', axis, view, end }
//       what the gizmo is drawn on ('' for none; 'end' the rope's pinned end, which only moves),
//       as which tool, the handle being dragged (0 X, 1 Y, 2 Z; -1 for none: drawn thick), where
//       the editor's view is ([left, bottom, right, top], -1 to 1) and the rope's pinned end
//       ([x, y, z], left out while it is free). What the gizmo is on is marked in orange: a cross
//       on the prop or the end, the box round the mesh
//   { op: 'studioPickProp', from: [x, y, z], direction: [x, y, z] }
//       a press in the view; answered
//   { op: 'studioPropPicked', thing, axis, at: [x, y, z], turn: [x, y, z] }
//       the thing pressed ('' for none), the gizmo's handle pressed (-1 for none) and where the
//       thing is now (its turn in degrees about x, y, z), so the editor drags from there
//
// The move arrows and the scale handles run along the world's axes; the rings turn about them.
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::prop_gizmo {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: draws the gizmo and answers a press (send: to the resource
// runtime).
void tick(void* local_entity, const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::studio::prop_gizmo
