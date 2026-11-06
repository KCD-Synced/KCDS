// A whip (Asset/Animation Studio): the game's own whip handle with a lash of
// the game's rope physics from its tip.
//
// - The handle: a BasicEntity showing the game's horse whip
//   (Objects/manmade/task_specific_props/animal_keeping/horse/whip.cgf), spawned without physics
//   (props/prop_entity spawn_mesh) and put where the hand is, turned with it (place_in_world:
//   the entity's SetWorldTM).
// - Its length: the handle's box (prop_entity world_bounds) while it still stands unturned where
//   it was spawned; its long side is the handle, the end farther from its origin the tip.
// - The lash: drawn by a RopeProp (the game's rope) whose start is held at the handle's tip; its
//   points come from our tapered lash (whip_lash.hpp) every frame, the game's rope kept still
//   (no gravity, all damping, no collisions) between them. The game's own rope, one mass along
//   its length, only hung and swung from the hand.
//
// Engine thread.
#pragma once

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "rope_prop.hpp"
#include "whip_lash.hpp"

namespace kcds::runtime::physical_props {

class WhipProp {
 public:
  WhipProp() = default;
  WhipProp(const WhipProp&) = delete;
  WhipProp& operator=(const WhipProp&) = delete;
  ~WhipProp();

  // A whip whose handle entity is named `name` (unique among the game's entities) with a lash
  // of `segments` and `length` metres, at `at`. False when the handle did not load.
  bool make(const std::string& name, int segments, float length, const float at[3]);

  // Puts the handle in the hand (a world place and turn, x y z w) and the lash's start at its
  // tip. The handle is measured on the first call (it stands unturned until then).
  void place(const float hand[3], const float turn[4]);

  // How the hand holds the handle: turned `pitch` degrees about the hand's x (its own axis runs
  // along the forearm, so 0 points the handle down the arm and 90 out of the fist) and
  // `twist` degrees about the hand's z after that.
  void set_grip(float pitch, float twist);

  // How the lash moves, in place of the shared settings (set_rope_physics): gravity, damping and
  // air drag (the lash's drag), collide (0: through the ground).
  void set_physics(const RopePhysics& physics) {
    own_physics_ = physics;
  }

  // The ground's height under the whip (metres up): the lash stays above it.
  void set_ground(float height) {
    ground_ = height;
  }

  // The lash's shape from an animation's keys (points from the handle's tip in its frame: along,
  // right, up), taken by the next place() instead of physics; empty: physics again. With `body`
  // (the holding body's turn, x y z w) the points are in the body's frame instead (forward,
  // right, up): the lash keeps its way when the hand turns, only its start follows the handle's
  // tip (a lash in the handle's frame swung all of itself with every turn of the wrist).
  void set_lash_shape(std::vector<std::array<float, 3>> shape, const float* body = nullptr) {
    lash_shape_ = std::move(shape);
    lash_in_body_ = body != nullptr;
    if (body) {
      for (int part = 0; part < 4; ++part) {
        lash_body_[part] = body[part];
      }
    }
  }

  // The body that holds it (the hand: left or right): with collide 1 (Ignore characters off)
  // the lash collides with that body's torso, head, arms and legs (capsules along its joints,
  // the holding forearm left out); null for none.
  void set_holder(void* body, bool left_hand) {
    holder_ = body;
    holder_left_ = left_hand;
  }

  // The handle's long side in its own space, as a unit: a swing test points it.
  const float* axis() const {
    return axis_;
  }

  void remove();

  bool made() const {
    return handle_ != nullptr;
  }

  int segments() const {
    return segments_;
  }

  float length() const {
    return length_;
  }

  // The lash's points in the world, its start (the handle's tip) first (segments + 1 of them).
  std::vector<float> lash_points() const {
    return model_.points();
  }

  // The handle's long side and up side in the world where it was placed last (unit vectors, the
  // posed lash's frame: its curl bends toward up); false before it was placed.
  bool handle_frame(float along[3], float up[3]) const {
    if (!framed_) {
      return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
      along[axis] = along_[axis];
      up[axis] = up_[axis];
    }
    return true;
  }

 private:
  std::vector<std::array<float, 3>> lash_shape_;
  // Whether lash_shape_ is in the holding body's frame (lash_body_, its turn) or the handle's.
  bool lash_in_body_ = false;
  float lash_body_[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float along_[3] = {0, 0, 0};
  float up_[3] = {0, 0, 0};
  bool framed_ = false;
  bool measure();

  std::string name_;
  void* handle_ = nullptr;
  bool measured_ = false;
  float spawn_[3] = {};
  float axis_[3] = {1, 0, 0};
  float tip_[3] = {0.6f, 0, 0};
  // The end the hand holds, in the handle's own space (measured with the tip).
  float butt_[3] = {0, 0, 0};
  // The grip (set_grip) as a turn in the hand's space; out of the fist at first.
  float grip_[4] = {0.70710678f, 0, 0, 0.70710678f};
  // The lash drawn tapered: the game's rope has one thickness, so three ropes of less and less
  // thickness draw it from the handle to the tip, each over its share of the lash's points
  // (kLashParts in whip_prop.cpp).
  RopeProp lash_[3];
  // The handle drawn as a short stout leather tube from its butt to its tip (the game's stick is
  // hidden).
  RopeProp handle_rope_;
  int part_start_[4] = {0, 0, 0, 0};
  // Whether each part took the lash's points last frame (logged when it changes).
  bool part_drawn_[3] = {true, true, true};
  int segments_ = 0;
  float length_ = 0.0f;
  // The lash as it moves (whip_lash.hpp), its settings (set_physics, else the shared ones), the
  // ground under it and when it moved last.
  WhipLash model_;
  std::optional<RopePhysics> own_physics_;
  float ground_ = -1.0e9f;
  void* holder_ = nullptr;
  bool holder_left_ = false;
  std::vector<LashBody> bodies_;
  int bodies_logged_ = 0;
  std::optional<std::chrono::steady_clock::time_point> moved_at_;
};

}  // namespace kcds::runtime::physical_props
