// The studio's skeleton view: the character's bones drawn over its body, every
// frame while it is on: a line from each joint to its parent and a small mark on each joint,
// over the scene (gizmos line without the depth test), the chosen joint in the accent colour
// with its name.
//
// The joints come from the character's skeleton (props/prop_entity joint_count, joint_parent,
// joint_pose_by_id: IDefaultSkeleton slots 1 and 2, the pose's absolute joints).
//
//   { op: 'studioBones', on, joint, view }   from the studio's editor: the view on or off, the
//                                            chosen joint's id (-1 for none) and where the
//                                            editor shows the world ([left, bottom, right, top],
//                                            -1 to 1): the bones are clipped to it with the free
//                                            camera's view (noclip::camera_view), so none is
//                                            drawn over the editor's panels (studio_view)
//   { op: 'studioPickJoint', from, direction }   a press in the editor's view: on a ring of the
//                                                chosen joint's turning gizmo, that ring; else
//                                                the joint nearest the line; answered
//                                                { op: 'studioJointPicked', id, ring } (id -1
//                                                for none, ring 0 X, 1 Y, 2 Z or -1)
//
// The chosen joint has a turning gizmo: a ring about each of its parent's axes (red X, green
// Y, blue Z, 15 cm across), studioBones' `ring` the one being turned (drawn thick).
//
// The editor gets the joints' names once the view is on ({ op: 'studioJoints', names: [...],
// parents: [...] }).
//
// handle_runtime_message: any thread; tick: engine thread.
#pragma once

#include <functional>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::studio::skeleton {

bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Every frame in the studio's world: draws the bones of the local character when the view is
// on, and tells the editor the joints once (send: to the resource runtime).
void tick(void* local_entity, const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::studio::skeleton
