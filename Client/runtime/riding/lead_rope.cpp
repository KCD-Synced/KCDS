#include "lead_rope.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/log.hpp"
#include "../props/prop_entity.hpp"
#include "../vfx/placed_objects_tick.hpp"
#include "native_horses.hpp"

namespace kcds::game_client {

namespace {

namespace entity = kcds::runtime::props::entity;

// The pieces: the game's plain cylinder (even along its height, z, without a physics proxy) drawn
// with the material of the game's tent ropes, so the pieces join into one smooth rope; the fishing
// stick drawn so is the last resort. The stick showed its taper and knots at every joint, a rope
// of joined pieces, and none of the game's own ropes is straight in its file (a tent's guy line
// slants, a laundry line sags, a rope maker's piece is coiled). A mesh with no fixed
// axis is taken along its longest side when it is straight there. How many pieces a rope has and
// how thick it is drawn (metres).
struct RopeMesh {
  const char* model;
  const char* material;
  // The axis its length runs along (0, 1, 2), -1 to learn it from its box.
  int axis;
};
constexpr char kRopeMaterial[] = "objects/manmade/structures/living/tents/rope_natural.mtl";
constexpr std::array<RopeMesh, 2> kRopeMeshes = {{
    {"objects/special/primitive_cylinder_noproxy.cgf", kRopeMaterial, 2},
    {"objects/manmade/task_specific_props/camping/stick_for_fish.cgf", kRopeMaterial, -1},
}};
// Straight enough: thinner across than this share of its length.
constexpr float kStraightShare = 0.12f;
constexpr int kSegments = 24;
constexpr float kThickness = 0.025f;
// The rope's length, a lead rope's (metres): nearer, it hangs; further, it pulls straight. How
// fast its hanging follows a change (a share of the way each second).
constexpr float kRopeLength = 3.2f;
constexpr float kSagFollow = 6.0f;
// The deepest it hangs, and how deep it hangs when pulled straight (metres).
constexpr float kMaxSag = 0.9f;
constexpr float kTautSag = 0.04f;
// Where on the bodies it is tied: where the horse's reins start at the bit (reinsRoot, then the
// rein belt, horse.chr), so the lead rope comes from its tack; the jaw or the head when a skeleton
// has no reins; the leader's right hand.
constexpr std::array<const char*, 8> kHorseHeads = {
    "reinsRoot", "ReinBelt_01", "Jaw", "jaw", "Bip01 Head", "head", "Head", "def_head"};
constexpr char kLeaderHand[] = "RightHand";

struct Vec {
  float x = 0;
  float y = 0;
  float z = 0;
};

Vec operator+(Vec a, Vec b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}

Vec operator-(Vec a, Vec b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}

Vec operator*(Vec a, float k) {
  return {a.x * k, a.y * k, a.z * k};
}

float length(Vec a) {
  return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}

// The mesh of a piece in its own frame: the axis it is long along (0, 1, 2), how long and how
// thick it is, and its box's centre.
struct MeshShape {
  bool known = false;
  int axis = 1;
  float length = 1;
  float thickness = 0.05f;
  Vec centre;
};
MeshShape shape;
// The rope mesh in use (kRopeMeshes), and whether the ropes' pieces are of an earlier one.
std::size_t mesh_index = 0;
bool mesh_changed = false;

struct Rope {
  std::vector<std::string> pieces;
  std::uint32_t generation = 0;
  // The rope's length and how deep it hangs now.
  float length = 0;
  float sag = 0;
  double placed_at = 0;
  // The horse's head joint that answered, "" until one did.
  std::string head;
  // The kRopeMeshes entry its pieces are of.
  std::size_t mesh = 0;
};
std::unordered_map<core::EntityId, Rope> ropes;
std::uint32_t generation = 0;

void remove_rope(Rope& rope) {
  for (const std::string& name : rope.pieces) {
    entity::remove(name);
  }
  rope.pieces.clear();
}

// Learns the mesh's shape from a spawned piece once the game has loaded it.
void learn_shape(void* piece) {
  float min[3];
  float max[3];
  if (shape.known || !entity::local_bounds(piece, min, max)) {
    return;
  }
  const float extent[3] = {max[0] - min[0], max[1] - min[1], max[2] - min[2]};
  const int fixed_axis = kRopeMeshes[mesh_index].axis;
  int axis = fixed_axis >= 0 ? fixed_axis : 0;
  for (int i = 1; fixed_axis < 0 && i < 3; ++i) {
    if (extent[i] > extent[axis]) {
      axis = i;
    }
  }
  float thickness = 0;
  for (int i = 0; i < 3; ++i) {
    if (i != axis) {
      thickness = std::max(thickness, extent[i]);
    }
  }
  if (extent[axis] <= 0.001f || thickness <= 0.0001f) {
    return;
  }
  if (fixed_axis < 0 && thickness > extent[axis] * kStraightShare &&
      mesh_index + 1 < kRopeMeshes.size()) {
    // Not a straight rope in its file: the next mesh, and every rope made again of it.
    log("LEAD ROPE mesh %s not straight (length=%.3f thickness=%.3f): the next",
        kRopeMeshes[mesh_index].model, extent[axis], thickness);
    ++mesh_index;
    mesh_changed = true;
    return;
  }
  shape.known = true;
  shape.axis = axis;
  shape.length = extent[axis];
  shape.thickness = thickness;
  shape.centre = {(min[0] + max[0]) / 2, (min[1] + max[1]) / 2, (min[2] + max[2]) / 2};
  log("LEAD ROPE mesh %s axis=%d length=%.3f thickness=%.3f", kRopeMeshes[mesh_index].model, axis,
      shape.length, shape.thickness);
}

// The shortest turn of unit vector `from` onto unit vector `to`, as (x, y, z, w).
void turn_onto(Vec from, Vec to, float rotation[4]) {
  const float dot = from.x * to.x + from.y * to.y + from.z * to.z;
  if (dot < -0.9999f) {
    // Half a turn about an axis across `from`.
    Vec across = std::fabs(from.x) < 0.9f ? Vec{1, 0, 0} : Vec{0, 1, 0};
    const Vec axis = {from.y * across.z - from.z * across.y, from.z * across.x - from.x * across.z,
                      from.x * across.y - from.y * across.x};
    const float size = length(axis);
    rotation[0] = axis.x / size;
    rotation[1] = axis.y / size;
    rotation[2] = axis.z / size;
    rotation[3] = 0;
    return;
  }
  const Vec cross = {from.y * to.z - from.z * to.y, from.z * to.x - from.x * to.z,
                     from.x * to.y - from.y * to.x};
  float q[4] = {cross.x, cross.y, cross.z, 1 + dot};
  const float size = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  for (int i = 0; i < 4; ++i) {
    rotation[i] = q[i] / size;
  }
}

// `v` turned by the quaternion (x, y, z, w).
Vec rotate(const float q[4], Vec v) {
  const Vec u = {q[0], q[1], q[2]};
  const float s = q[3];
  const Vec t = {2 * (u.y * v.z - u.z * v.y), 2 * (u.z * v.x - u.x * v.z),
                 2 * (u.x * v.y - u.y * v.x)};
  return v + t * s + Vec{u.y * t.z - u.z * t.y, u.z * t.x - u.x * t.z, u.x * t.y - u.y * t.x};
}

// A point of the hanging rope from `a` to `b`, `t` from 0 to 1 along it: the straight line,
// lowered by a parabola `sag` deep in the middle.
Vec rope_point(Vec a, Vec b, float sag, float t) {
  return a + (b - a) * t - Vec{0, 0, sag * 4 * t * (1 - t)};
}

bool joint_at(void* body, const char* joint, Vec& out) {
  float position[3];
  float rotation[4];
  if (!entity::joint_pose(body, joint, position, rotation)) {
    return false;
  }
  out = {position[0], position[1], position[2]};
  return true;
}

// The horse's head, by the first of kHorseHeads its skeleton has.
bool head_of(void* horse, Rope& rope, Vec& out) {
  if (!rope.head.empty()) {
    return joint_at(horse, rope.head.c_str(), out);
  }
  for (const char* name : kHorseHeads) {
    if (joint_at(horse, name, out)) {
      rope.head = name;
      log("LEAD ROPE horse head joint %s", name);
      return true;
    }
  }
  return false;
}

// Places one rope for this frame; false when an end is not there.
bool place_rope(core::EntityId id, const NativeHorse& horse, Rope& rope, double now) {
  void* horse_entity = entity::by_id(horse.engine_id);
  void* leader = placed_body_entity(horse.lead);
  Vec head;
  Vec hand;
  if (!horse_entity || !leader || !head_of(horse_entity, rope, head) ||
      !joint_at(leader, entity::body_joint_name(leader, kLeaderHand).c_str(), hand)) {
    return false;
  }
  const float distance = length(hand - head);
  if (!rope.pieces.empty() && rope.mesh != mesh_index) {
    // Pieces of a mesh that was not straight: made again of the current one.
    remove_rope(rope);
  }
  if (rope.pieces.empty()) {
    rope.mesh = mesh_index;
    rope.length = kRopeLength;
    rope.sag = kMaxSag * 0.5f;
    ++generation;
    for (int i = 0; i < kSegments; ++i) {
      const std::string name = "kcds_lead_rope_" + std::to_string(id) + "_" +
                               std::to_string(generation) + "_" + std::to_string(i);
      const float at[3] = {head.x, head.y, head.z};
      const RopeMesh& mesh = kRopeMeshes[mesh_index];
      if (entity::spawn_mesh(name, at, mesh.model, mesh.material)) {
        rope.pieces.push_back(name);
      }
    }
    rope.placed_at = now;
  }
  // How deep a rope of its length hangs between ends this far apart (a parabola's arc), followed
  // smoothly.
  const float spare = std::max(rope.length - distance, 0.0f);
  const float wanted =
      std::clamp(std::sqrt(3.0f * std::max(distance, 0.1f) * spare / 8.0f), kTautSag, kMaxSag);
  const float step = float(std::clamp((now - rope.placed_at) * kSagFollow, 0.0, 1.0));
  rope.sag += (wanted - rope.sag) * step;
  rope.placed_at = now;
  const Vec axis_vector = shape.axis == 0   ? Vec{1, 0, 0}
                          : shape.axis == 1 ? Vec{0, 1, 0}
                                            : Vec{0, 0, 1};
  for (std::size_t i = 0; i < rope.pieces.size(); ++i) {
    void* piece = entity::find(rope.pieces[i]);
    if (!piece) {
      continue;
    }
    learn_shape(piece);
    if (mesh_changed) {
      // This mesh was not straight: the next tick makes the rope of the next.
      return true;
    }
    const float t0 = float(i) / float(rope.pieces.size());
    const float t1 = float(i + 1) / float(rope.pieces.size());
    const Vec from = rope_point(head, hand, rope.sag, t0);
    const Vec to = rope_point(head, hand, rope.sag, t1);
    const float span = length(to - from);
    if (!shape.known || span < 0.0001f) {
      entity::hide(piece, true);
      continue;
    }
    entity::hide(piece, false);
    float rotation[4];
    turn_onto(axis_vector, (to - from) * (1.0f / span), rotation);
    // A little longer than its stretch, so the pieces meet without gaps at the bends.
    const float along = span * 1.04f / shape.length;
    const float across = kThickness / shape.thickness;
    float scale[3] = {across, across, across};
    scale[shape.axis] = along;
    entity::set_scale(piece, scale);
    // The mesh's centre goes to the middle of its stretch.
    const Vec centre = {shape.centre.x * scale[0], shape.centre.y * scale[1],
                        shape.centre.z * scale[2]};
    const Vec middle = (from + to) * 0.5f;
    const Vec origin = middle - rotate(rotation, centre);
    const float position[3] = {origin.x, origin.y, origin.z};
    entity::place_rotated(piece, position, rotation);
  }
  return true;
}

}  // namespace

void tick_lead_ropes(double now) {
  if (mesh_changed) {
    // The pieces of a mesh that was not straight go; place_rope makes them of the next.
    mesh_changed = false;
    for (auto& [id, rope] : ropes) {
      remove_rope(rope);
    }
  }
  for (auto it = ropes.begin(); it != ropes.end();) {
    const auto horse = native_horses.find(it->first);
    if (horse == native_horses.end() || horse->second.lead == 0) {
      remove_rope(it->second);
      it = ropes.erase(it);
    } else {
      ++it;
    }
  }
  for (const auto& [id, horse] : native_horses) {
    if (horse.lead == 0) {
      continue;
    }
    Rope& rope = ropes[id];
    if (!place_rope(id, horse, rope, now)) {
      // An end is not in this game now (out of sight, loading): no rope until both are.
      remove_rope(rope);
    }
  }
}

void clear_lead_ropes() {
  for (auto& [id, rope] : ropes) {
    remove_rope(rope);
  }
  ropes.clear();
}

}  // namespace kcds::game_client
