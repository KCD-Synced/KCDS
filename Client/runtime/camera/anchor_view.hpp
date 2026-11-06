// A camera of our own: a view of the engine's view system that follows an anchor entity (WHGame
// 1.5.5). Used by NoClip (noclip/) and the CharacterCreator's close framing (character_creator/).
//
// The view system (CViewSystem) is gFramework vtable + 232: slot 1 CreateView, 2 RemoveView(id),
// 5 SetActiveView, 6 GetView(id), 9 GetViewId; the active view's id is at + 96. A view (CView)
// keeps its SViewParams at + 20 (slot 6 SetCurrentParams, 7 GetCurrentParams): the position at
// floats 0-2, the rotation quaternion at 3-6, the field of view in radians at 12, and the last
// frame's position, rotation and field of view at 45-52. A view takes its position and rotation
// from the entity it is linked to (its id at + 16) every frame (CView::Update); a view
// linked to nothing is never updated. So the anchor is a bodyless TagPoint spawned with the
// entity system's SpawnEntity (slot 12, SEntitySpawnParams built), moved with
// SetWorldTM (entity slot 30); its columns are the camera's right, forward and up. The view that
// was active before comes back when the camera ends, also while the pause screen films the
// player page (its own camera then steps aside).
//
// Yaw is in degrees from +Y, rising to the left; pitch is in degrees, positive up; roll is in
// degrees about the forward, after both. The view that was active comes back without roll.
//
// Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::camera {

struct Natives {
  void* (*framework)() = nullptr;      // gFramework (IGameFramework)
  void* (*entity_system)() = nullptr;  // IEntitySystem
  void* spawn_params_init = nullptr;   // SEntitySpawnParams::SEntitySpawnParams
};

struct Pose {
  double x = 0;
  double y = 0;
  double z = 0;
  double yaw = 0;
  double pitch = 0;
  // Degrees about the line of sight, positive turns the view's right side down.
  double roll = 0;
};

// The heading, elevation and roll of a rotation quaternion (x, y, z, w) turning +Y; the position
// is left at 0.
Pose pose_of_rotation(double x, double y, double z, double w);

class AnchorView {
 public:
  explicit AnchorView(const char* anchor_name) : anchor_name_(anchor_name) {
  }

  // Makes a camera at the pose the active view; the pose and field of view (degrees) of the view
  // that was active come back through the out arguments when given. False when the view system or
  // the anchor is unavailable.
  bool begin(const Natives& natives, Pose* previous_pose, double* previous_fov);

  // Places the camera; false when its view or anchor is gone (the camera has ended).
  bool place(const Pose& pose, double fov_degrees);

  // Gives the view that was active back and removes the camera.
  void end();

  bool active() const {
    return anchor_id_ != 0;
  }

 private:
  void* views() const;
  void* anchor() const;

  const char* anchor_name_;
  Natives natives_;
  unsigned previous_view_ = 0;
  unsigned camera_view_ = 0;
  std::uint32_t anchor_id_ = 0;
};

}  // namespace kcds::runtime::camera
