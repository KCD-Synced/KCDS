#include "studio_skeleton.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "../core/log.hpp"
#include "../gizmos/gizmos.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "studio_view.hpp"

namespace kcds::runtime::studio::skeleton {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace entity = props::entity;

using view::kNear;
using view::View;
using PickRequest = view::Ray;

constexpr float kBone[4] = {0.55f, 0.85f, 1.0f, 0.95f};
constexpr float kChosen[4] = {0.94f, 0.66f, 0.23f, 1.0f};
constexpr float kBoneThickness = 2.0f;
constexpr float kChosenThickness = 4.0f;
constexpr float kMarkRadius = 0.012f;
constexpr float kLabelSize = 1.3f;
// A press picks the joint nearest its line, within this angle (radians) of it.
constexpr double kPickAngle = 0.035;
constexpr float kRingRadius = 0.15f;

std::atomic<bool> showing{false};
std::atomic<int> chosen{-1};
// The ring the editor is turning the chosen joint with (drawn thick), -1 for none.
std::atomic<int> active_ring{-1};
// The editor asks again each time it turns the bones on (it may have been reloaded).
std::atomic<bool> tell_again{true};
// The body whose joints the editor has, and their parents (frame thread only).
void* told_about = nullptr;
std::vector<int> parents_of;
std::vector<std::string> names_of;
// This frame's joint places (kept so a frame makes no new buffers).
std::vector<float> places;
std::vector<bool> known;
std::vector<bool> in_part;

std::mutex requests_mutex;
std::optional<PickRequest> pick_request;
// Joints the editor asked about (studioJointInfo): their names.
std::optional<std::vector<std::string>> info_request;

// The joint nearest the press's line, within kPickAngle; -1 for none. The root's helpers are
// left out, as they are not drawn.
int nearest_joint(const PickRequest& request, const std::vector<float>& places,
                  const std::vector<bool>& known) {
  int best = -1;
  double best_angle = kPickAngle;
  const int count = int(known.size());
  for (int id = 0; id < count; ++id) {
    const int parent = parents_of[id];
    const bool helper = parent >= 0 && parents_of[parent] < 0;
    if (!known[id] || helper || parent < 0) {
      continue;
    }
    const float* at = &places[std::size_t(id) * 3];
    const double offset[3] = {at[0] - request.from[0], at[1] - request.from[1],
                              at[2] - request.from[2]};
    const double along = offset[0] * request.direction[0] + offset[1] * request.direction[1] +
                         offset[2] * request.direction[2];
    if (along <= kNear) {
      continue;
    }
    double across = 0;
    for (int axis = 0; axis < 3; ++axis) {
      const double rest = offset[axis] - request.direction[axis] * along;
      across += rest * rest;
    }
    const double angle = std::atan2(std::sqrt(across), along);
    if (angle < best_angle) {
      best_angle = angle;
      best = id;
    }
  }
  return best;
}

// The turning gizmo of the chosen joint (joint `id` at `at`): a ring about each of its parent's
// axes (X, Y, Z, as the editor's Turn X/Y/Z turn it; the world's for the root). False without the
// parent's pose.
bool rings_of(void* entity, int id, const float* at, view::Rings& rings) {
  float turn[4] = {0, 0, 0, 1};
  const int parent = parents_of[id];
  if (parent >= 0) {
    float position[3];
    if (!entity::joint_pose_by_id(entity, parent, position, turn)) {
      return false;
    }
  }
  rings = view::rings_about(at, turn, kRingRadius);
  return true;
}

// A vector in the body's own frame: right (x), forward (y), up (z), as the character faces.
std::array<float, 3> in_body(const float body_turn[4], const float world[3]) {
  float inverse[4];
  pose_math::conjugate(body_turn, inverse);
  float out[3];
  pose_math::rotate(inverse, world, out);
  return {out[0], out[1], out[2]};
}

sdk::Value vector_value(const std::array<float, 3>& v) {
  auto rounded = [](float x) { return sdk::Value(std::round(double(x) * 1000.0) / 1000.0); };
  return sdk::Value(sdk::Value::Array{rounded(v[0]), rounded(v[1]), rounded(v[2])});
}

// What the editor's assistant needs to pose a joint (studioJointInfo): per joint asked, where
// it is from the body's root, its bone (to its longest child), its parent's axes (the axes the
// joint's turns are about) and how far a turn of +30 degrees about each moves the bone's end,
// all in the body's frame (right, forward, up), metres.
sdk::Value joint_info(void* entity, const std::vector<std::string>& wanted) {
  float body_position[3];
  float body_turn[4];
  entity::world_pose(entity, body_position, body_turn);
  const int count = int(parents_of.size());
  sdk::Value::Array joints;
  for (const std::string& name : wanted) {
    const int id = entity::joint_id(entity, name.c_str());
    if (id < 0 || id >= count || !known[id]) {
      joints.push_back(
          sdk::Value(sdk::Value::Map{{"name", sdk::Value(name)}, {"found", sdk::Value(false)}}));
      continue;
    }
    const float* at = &places[std::size_t(id) * 3];
    // The bone: to the child farthest away (the chain's next joint, not a roll helper).
    int child = -1;
    float longest = 0.0f;
    for (int other = 0; other < count; ++other) {
      if (parents_of[other] != id || !known[other]) {
        continue;
      }
      const float* to = &places[std::size_t(other) * 3];
      const float length =
          std::sqrt((to[0] - at[0]) * (to[0] - at[0]) + (to[1] - at[1]) * (to[1] - at[1]) +
                    (to[2] - at[2]) * (to[2] - at[2]));
      if (length > longest) {
        longest = length;
        child = other;
      }
    }
    float parent_turn[4] = {body_turn[0], body_turn[1], body_turn[2], body_turn[3]};
    const int parent = parents_of[id];
    if (parent >= 0) {
      float parent_position[3];
      entity::joint_pose_by_id(entity, parent, parent_position, parent_turn);
    }
    const float offset[3] = {at[0] - body_position[0], at[1] - body_position[1],
                             at[2] - body_position[2]};
    sdk::Value::Map joint{
        {"name", sdk::Value(name)},
        {"found", sdk::Value(true)},
        {"id", sdk::Value(std::int64_t(id))},
        {"parent", sdk::Value(parent >= 0 && parent < int(names_of.size()) ? names_of[parent]
                                                                           : std::string())},
        {"position", vector_value(in_body(body_turn, offset))}};
    float bone[3] = {0, 0, 0};
    if (child >= 0) {
      const float* to = &places[std::size_t(child) * 3];
      for (int axis = 0; axis < 3; ++axis) {
        bone[axis] = to[axis] - at[axis];
      }
      joint.emplace_back("child", sdk::Value(names_of[child]));
      joint.emplace_back("bone", vector_value(in_body(body_turn, bone)));
    }
    const float units[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const char* axis_names[3] = {"x", "y", "z"};
    sdk::Value::Map axes;
    sdk::Value::Map moves;
    for (int axis = 0; axis < 3; ++axis) {
      float world_axis[3];
      pose_math::rotate(parent_turn, units[axis], world_axis);
      axes.emplace_back(axis_names[axis], vector_value(in_body(body_turn, world_axis)));
      if (child >= 0) {
        // The bone turned +30 degrees about this axis: where its end goes.
        const float half = 15.0f * 3.14159265f / 180.0f;
        const float turn[4] = {world_axis[0] * std::sin(half), world_axis[1] * std::sin(half),
                               world_axis[2] * std::sin(half), std::cos(half)};
        float turned[3];
        pose_math::rotate(turn, bone, turned);
        const float moved[3] = {turned[0] - bone[0], turned[1] - bone[1], turned[2] - bone[2]};
        moves.emplace_back(axis_names[axis], vector_value(in_body(body_turn, moved)));
      }
    }
    joint.emplace_back("axes", sdk::Value(axes));
    if (child >= 0) {
      joint.emplace_back("plus30moves", sdk::Value(moves));
    }
    joints.push_back(sdk::Value(joint));
  }
  // The body's place and its forward in the world, so the assistant can put the camera by a
  // joint (editor_frame).
  const float ahead[3] = {0.0f, 1.0f, 0.0f};
  float forward[3];
  pose_math::rotate(body_turn, ahead, forward);
  const sdk::Value place = vector_value({body_position[0], body_position[1], body_position[2]});
  const sdk::Value facing = vector_value({forward[0], forward[1], forward[2]});
  return sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioJointInfo")},
                                    {"frame", sdk::Value("right, forward, up (metres)")},
                                    {"place", place},
                                    {"forward", facing},
                                    {"joints", sdk::Value(joints)}});
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "studioJointInfo") {
    std::vector<std::string> names;
    const sdk::Value* list = integration::script_field(message, "names");
    if (list && list->tag() == sdk::Tag::Array) {
      for (const sdk::Value& name : list->as_array()) {
        if (name.tag() == sdk::Tag::Str && names.size() < 64) {
          names.push_back(name.as_str());
        }
      }
    }
    std::lock_guard<std::mutex> lock(requests_mutex);
    info_request = names;
    return true;
  }
  if (operation == "studioPickJoint") {
    PickRequest request;
    if (view::read_ray(message, request)) {
      std::lock_guard<std::mutex> lock(requests_mutex);
      pick_request = request;
    }
    return true;
  }
  if (operation != "studioBones") {
    return false;
  }
  const sdk::Value* on = integration::script_field(message, "on");
  const bool show = on && on->tag() == sdk::Tag::Bool && on->as_bool();
  if (show && !showing.load()) {
    tell_again.store(true);
  }
  showing.store(show);
  const sdk::Value* joint = integration::script_field(message, "joint");
  double id = -1;
  if (!joint || !view::number_of(*joint, id)) {
    id = -1;
  }
  chosen.store(int(id));
  const sdk::Value* ring = integration::script_field(message, "ring");
  double ring_number = -1;
  if (!ring || !view::number_of(*ring, ring_number)) {
    ring_number = -1;
  }
  active_ring.store(int(ring_number));
  double rect[4];
  if (view::numbers_of(message, "view", rect, 4)) {
    view::set_viewport(view::Rect{rect[0], rect[1], rect[2], rect[3]});
  }
  return true;
}

void tick(void* local_entity, const std::function<bool(const sdk::Value&)>& send) {
  if (!showing.load() || !local_entity) {
    return;
  }
  const int count = entity::joint_count(local_entity);
  if (count <= 0) {
    return;
  }
  if (tell_again.exchange(false) || told_about != local_entity || int(parents_of.size()) != count) {
    told_about = local_entity;
    parents_of.assign(std::size_t(count), -1);
    names_of = entity::joint_names(local_entity);
    names_of.resize(std::size_t(count));
    sdk::Value::Array names;
    sdk::Value::Array parents;
    for (int id = 0; id < count; ++id) {
      parents_of[id] = entity::joint_parent(local_entity, id);
      names.push_back(sdk::Value(names_of[id]));
      parents.push_back(sdk::Value(std::int64_t(parents_of[id])));
    }
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioJoints")},
                                    {"names", sdk::Value(names)},
                                    {"parents", sdk::Value(parents)}}));
    game_client::log("STUDIO skeleton: %d joints told to the editor", count);
  }
  // One pass over the joints: the character, its pose and the body's place are looked up once.
  places.resize(std::size_t(count) * 3);
  if (!entity::joint_places(local_entity, count, places.data(), known)) {
    return;
  }
  std::optional<PickRequest> request;
  {
    std::lock_guard<std::mutex> lock(requests_mutex);
    request.swap(pick_request);
  }
  const int picked_now = chosen.load();
  view::Rings rings;
  const bool with_rings =
      picked_now >= 0 && picked_now < count && known[picked_now] &&
      rings_of(local_entity, picked_now, &places[std::size_t(picked_now) * 3], rings);
  std::optional<std::vector<std::string>> asked;
  {
    std::lock_guard<std::mutex> lock(requests_mutex);
    asked.swap(info_request);
  }
  if (asked) {
    send(joint_info(local_entity, *asked));
  }
  if (request) {
    // A press on a ring of the chosen joint turns it (the editor drags); else it picks a joint.
    const int ring = with_rings ? view::ring_hit(rings, *request) : -1;
    const int id = ring >= 0 ? picked_now : nearest_joint(*request, places, known);
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value("studioJointPicked")},
                                    {"id", sdk::Value(std::int64_t(id))},
                                    {"ring", sdk::Value(std::int64_t(ring))}}));
  }
  View shown;
  if (!view::current(shown)) {
    return;
  }
  const int picked = chosen.load();
  // The chosen joint's part of the body: the joint and every joint below it (a parent comes
  // before its children in the skeleton's order), what turning the joint moves.
  in_part.assign(std::size_t(count), false);
  for (int id = 0; id < count; ++id) {
    const int parent = parents_of[id];
    in_part[id] = id == picked || (parent >= 0 && parent < id && in_part[parent]);
  }
  for (int id = 0; id < count; ++id) {
    if (!known[id]) {
      continue;
    }
    const float* at = &places[std::size_t(id) * 3];
    const float* colour = in_part[id] ? kChosen : kBone;
    const int parent = parents_of[id];
    // The root stands on the floor and holds the helpers (IK targets, face poses): a line from it
    // to each would hide the body, so the bones start below it.
    const bool from_root = parent >= 0 && parents_of[parent] < 0;
    if (parent >= 0 && parent < count && known[parent] && !from_root) {
      // The bone into the chosen joint and the bones below it are drawn thick.
      const float thickness = in_part[id] ? kChosenThickness : kBoneThickness;
      view::clipped_line(shown, &places[std::size_t(parent) * 3], at, colour, thickness);
    }
    if ((!from_root || id == picked) && view::inside(shown, at)) {
      gizmos::circle(at, id == picked ? kMarkRadius * 2.5f : kMarkRadius, colour);
    }
  }
  if (with_rings && picked == picked_now) {
    view::draw_rings(shown, rings, active_ring.load());
  }
  if (picked >= 0 && picked < count && known[picked]) {
    const float* at = &places[std::size_t(picked) * 3];
    const float above[3] = {at[0], at[1], at[2] + 0.06f};
    if (picked < int(names_of.size()) && view::inside(shown, above)) {
      gizmos::label(above, kLabelSize, kChosen, names_of[picked].c_str());
    }
  }
}

}  // namespace kcds::runtime::studio::skeleton
