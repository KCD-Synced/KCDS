// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "whip_prop.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../core/log.hpp"
#include "../props/pose_math.hpp"
#include "../physics/local_world.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::physical_props {
namespace {

namespace entity = props::entity;

constexpr const char* kHandleMesh =
    "Objects/manmade/task_specific_props/animal_keeping/horse/whip.cgf";
// A real lash is about 1.5 cm across at the handle and a few millimetres at its tip. The game's
// rope draws one thickness (one of 14 mm looked like a hose), so three ropes
// draw it: the first kLashParts' share of the lash at its thickness, then the next, to the tip.
struct LashPart {
  float share;
  float thickness;
};
constexpr LashPart kLashParts[3] = {{0.4f, 0.016f}, {0.3f, 0.010f}, {0.3f, 0.005f}};
// How far up from its end the fist holds the handle (metres).
constexpr float kGripFromButt = 0.05f;
// The whip's look: leather (the game's own material; the rope's twisted light rope looked
// wrong on a lash), round tubes of eight sides, and a stout handle of its own: the game's horse
// whip is a thin stick 0.6 m long (a dotted line between the fist and the lash, live).
// The stick is still made, hidden, for the handle's axes (measure), so the grip's
// settings keep their meaning.
constexpr const char* kLashMaterial = "Materials/substance/leather_old";
// The handle in the same dark leather (the light cowhide looked like a wooden lath).
constexpr const char* kHandleMaterial = kLashMaterial;
constexpr int kTubeSides = 8;
constexpr float kHandleLength = 0.30f;
constexpr float kHandleThickness = 0.026f;

// A lash shape in the body's frame (points from the handle's tip: forward, right, up) bent out of
// the handle's line: its first pieces go on from the handle (`along`, in the world) and turn into
// the shape's own ways over kOutOfHandle of them, so the lash leaves the handle in a curve, not
// at a corner (a 90° corner where the handle pointed down and the lash ahead).
constexpr float kOutOfHandle[] = {0.15f, 0.4f, 0.7f, 0.9f};

std::vector<std::array<float, 3>> out_of_handle(const std::vector<std::array<float, 3>>& shape,
                                                const float along[3], const float forward[3],
                                                const float up[3]) {
  const float right[3] = {forward[1] * up[2] - forward[2] * up[1],
                          forward[2] * up[0] - forward[0] * up[2],
                          forward[0] * up[1] - forward[1] * up[0]};
  // The handle's way in the body's frame.
  const float handle[3] = {along[0] * forward[0] + along[1] * forward[1] + along[2] * forward[2],
                           along[0] * right[0] + along[1] * right[1] + along[2] * right[2],
                           along[0] * up[0] + along[1] * up[1] + along[2] * up[2]};
  std::vector<std::array<float, 3>> out;
  out.reserve(shape.size());
  std::array<float, 3> before{};
  std::array<float, 3> placed{};
  const std::size_t blended = sizeof(kOutOfHandle) / sizeof(kOutOfHandle[0]);
  for (std::size_t index = 0; index < shape.size(); ++index) {
    std::array<float, 3> piece{};
    for (int axis = 0; axis < 3; ++axis) {
      piece[std::size_t(axis)] = shape[index][std::size_t(axis)] - before[std::size_t(axis)];
    }
    before = shape[index];
    const float length = std::sqrt(piece[0] * piece[0] + piece[1] * piece[1] + piece[2] * piece[2]);
    if (index < blended && length > 1e-6f) {
      const float share = kOutOfHandle[index];
      std::array<float, 3> way{};
      for (int axis = 0; axis < 3; ++axis) {
        way[std::size_t(axis)] =
            handle[axis] * (1.0f - share) + piece[std::size_t(axis)] / length * share;
      }
      const float size = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
      if (size > 1e-6f) {
        for (int axis = 0; axis < 3; ++axis) {
          piece[std::size_t(axis)] = way[std::size_t(axis)] / size * length;
        }
      }
    }
    for (int axis = 0; axis < 3; ++axis) {
      placed[std::size_t(axis)] += piece[std::size_t(axis)];
    }
    out.push_back(placed);
  }
  return out;
}

// The body parts a lash collides with: capsules between joints (the skeleton's names) and how
// thick they are round (metres), a man's build.
struct BodyPart {
  const char* from;
  const char* to;
  float radius;
  // Left out for the arm that holds the whip (its lash leaves the hand there).
  int holding_arm;  // 0 none, 1 the right arm, 2 the left arm
};
constexpr BodyPart kBodyParts[] = {
    {"Hips", "Spine2", 0.17f, 0},
    {"Spine2", "Neck", 0.18f, 0},
    {"Neck", "Head", 0.07f, 0},
    {"Head", "HeadEnd", 0.11f, 0},
    {"RightArm", "RightForeArm", 0.06f, 0},
    {"RightForeArm", "RightHand", 0.05f, 1},
    {"LeftArm", "LeftForeArm", 0.06f, 0},
    {"LeftForeArm", "LeftHand", 0.05f, 2},
    {"RightUpLeg", "RightLeg", 0.09f, 0},
    {"RightLeg", "RightFoot", 0.07f, 0},
    {"LeftUpLeg", "LeftLeg", 0.09f, 0},
    {"LeftLeg", "LeftFoot", 0.07f, 0},
};

// How far round the hand other bodies are looked for (the lash with its handle reaches about
// 1.9 m; a body's place is its feet).
constexpr float kBodiesReach = 3.0f;

// A body's capsules into `parts`; `holding` 1 or 2 leaves the right or left forearm out.
void add_body(void* body, int holding, std::vector<LashBody>& parts) {
  float turn[4];
  for (const BodyPart& part : kBodyParts) {
    if (holding != 0 && part.holding_arm == holding) {
      continue;
    }
    LashBody capsule;
    capsule.radius = part.radius;
    capsule.own = holding != 0;
    const std::string from = entity::body_joint_name(body, part.from);
    const std::string to = entity::body_joint_name(body, part.to);
    if (entity::joint_pose(body, from.c_str(), capsule.a, turn) &&
        entity::joint_pose(body, to.c_str(), capsule.b, turn)) {
      parts.push_back(capsule);
    }
  }
}

// The bodies in the lash's way: the holder's (its holding forearm left out) and every other
// character with a skeleton within kBodiesReach of the hand (players and NPCs).
std::vector<LashBody> bodies_of(void* holder, bool left_hand, const float hand[3]) {
  std::vector<LashBody> parts;
  add_body(holder, left_hand ? 2 : 1, parts);
  for (void* other : physics::physical_entities_near(hand, kBodiesReach)) {
    const bool character =
        other != holder && entity::has_joint(other, entity::body_joint_name(other, "Hips").c_str());
    if (character) {
      add_body(other, 0, parts);
    }
  }
  return parts;
}

// How deep a point is inside a capsule (metres; 0 outside).
float depth_in(const float* point, const LashBody& body) {
  float line[3];
  float from_a[3];
  for (int axis = 0; axis < 3; ++axis) {
    line[axis] = body.b[axis] - body.a[axis];
    from_a[axis] = point[axis] - body.a[axis];
  }
  const float length = line[0] * line[0] + line[1] * line[1] + line[2] * line[2];
  const float dot = from_a[0] * line[0] + from_a[1] * line[1] + from_a[2] * line[2];
  const float share = length > 1e-8f ? std::clamp(dot / length, 0.0f, 1.0f) : 0.0f;
  float away = 0.0f;
  for (int axis = 0; axis < 3; ++axis) {
    const float part = point[axis] - (body.a[axis] + line[axis] * share);
    away += part * part;
  }
  return std::max(0.0f, body.radius - std::sqrt(away));
}

// Every two seconds, the deepest a lash point went into a body and how many were inside after
// a step (the lash's collision checked live: none should be).
void log_inside(const std::vector<float>& points, const std::vector<LashBody>& bodies) {
  static auto due = std::chrono::steady_clock::now();
  static float deepest = 0.0f;
  static int inside = 0;
  static int on_own = 0;
  static int on_others = 0;
  for (std::size_t point = 2; point < points.size() / 3; ++point) {
    for (const LashBody& body : bodies) {
      LashBody skin = body;
      skin.radius += 0.02f;
      const float depth = depth_in(&points[point * 3], body);
      if (depth > 0.005f) {
        ++inside;
        deepest = std::max(deepest, depth);
      } else if (depth_in(&points[point * 3], skin) > 0.0f) {
        ++(body.own ? on_own : on_others);
      }
    }
  }
  const auto now = std::chrono::steady_clock::now();
  // Told only when the lash hit someone else or went into a body (its own skin it touches
  // all the time).
  if (now >= due) {
    if (on_others > 0 || inside > 0) {
      game_client::log(
          "WHIP: in 2 s lash points on the holder's skin %d, on others' %d, inside %d (deepest "
          "%.3f m)",
          on_own, on_others, inside, deepest);
    }
    due = now + std::chrono::seconds(2);
    deepest = 0.0f;
    inside = 0;
    on_own = 0;
    on_others = 0;
  }
}

}  // namespace

WhipProp::~WhipProp() {
  remove();
}

bool WhipProp::make(const std::string& name, int segments, float length, const float at[3]) {
  remove();
  handle_ = entity::spawn_mesh(name, at, kHandleMesh, "");
  if (!handle_) {
    game_client::log("WHIP: the handle %s did not load", kHandleMesh);
    return false;
  }
  name_ = name;
  measured_ = false;
  for (int axis = 0; axis < 3; ++axis) {
    spawn_[axis] = at[axis];
  }
  // The game's ropes only draw the lash: still between our points (whip_lash.hpp).
  RopePhysics still;
  still.gravity = 0.0f;
  still.damping = 5.0f;
  still.collide = 0;
  segments_ = std::max(3, segments);
  length_ = length;
  const float segment = length / float(segments_);
  part_start_[0] = 0;
  for (int part = 0; part < 3; ++part) {
    // Each part at least one segment; the last one ends at the tip.
    int end = part == 2 ? segments_
                        : part_start_[part] +
                              std::max(1, int(std::lround(kLashParts[part].share * segments_)));
    end = std::min(end, segments_ - (2 - part));
    part_start_[part + 1] = end;
    const int count = end - part_start_[part];
    lash_[part].make(count, segment * float(count), at, kLashMaterial, kLashParts[part].thickness,
                     kTubeSides);
    lash_[part].set_own_physics(still);
  }
  handle_rope_.make(2, kHandleLength, at, kHandleMaterial, kHandleThickness, kTubeSides);
  handle_rope_.set_own_physics(still);
  model_.reset(segments_, length, at);
  moved_at_.reset();
  return true;
}

bool WhipProp::measure() {
  float low[3];
  float high[3];
  if (!entity::world_bounds(handle_, low, high)) {
    return false;
  }
  int longest = 0;
  float sizes[3];
  for (int axis = 0; axis < 3; ++axis) {
    low[axis] -= spawn_[axis];
    high[axis] -= spawn_[axis];
    sizes[axis] = high[axis] - low[axis];
    if (sizes[axis] > sizes[longest]) {
      longest = axis;
    }
  }
  const bool far_high = std::fabs(high[longest]) >= std::fabs(low[longest]);
  for (int axis = 0; axis < 3; ++axis) {
    axis_[axis] = 0;
    tip_[axis] = 0.5f * (low[axis] + high[axis]);
  }
  axis_[longest] = far_high ? 1.0f : -1.0f;
  tip_[longest] = far_high ? high[longest] : low[longest];
  // The end the hand holds: the other end of the long side (the handle's pivot is its middle).
  for (int axis = 0; axis < 3; ++axis) {
    butt_[axis] = 0.5f * (low[axis] + high[axis]);
  }
  butt_[longest] = far_high ? low[longest] : high[longest];
  // Our handle (kHandleLength) ends where the lash starts; the game's long stick is hidden.
  for (int axis = 0; axis < 3; ++axis) {
    tip_[axis] = butt_[axis] + axis_[axis] * kHandleLength;
  }
  entity::hide(handle_, true);
  measured_ = true;
  return true;
}

void WhipProp::set_grip(float pitch, float twist) {
  constexpr float kHalfRadians = 3.14159265f / 360.0f;
  const float about_x[4] = {std::sin(pitch * kHalfRadians), 0, 0, std::cos(pitch * kHalfRadians)};
  const float about_z[4] = {0, 0, std::sin(twist * kHalfRadians), std::cos(twist * kHalfRadians)};
  pose_math::multiply(about_x, about_z, grip_);
}

void WhipProp::place(const float hand[3], const float hand_turn[4]) {
  if (!handle_) {
    return;
  }
  if (!measured_ && !measure()) {
    return;
  }
  // The handle's turn: the hand's, then the grip in the hand's space.
  float turn[4];
  pose_math::multiply(hand_turn, grip_, turn);
  // The handle's butt in the hand: its pivot that far back along its turned long side. The fist
  // grips it a little up from its end (kGripFromButt), as a hand holds a whip.
  float gripped[3];
  for (int axis = 0; axis < 3; ++axis) {
    gripped[axis] = butt_[axis] + axis_[axis] * kGripFromButt;
  }
  float butt[3];
  pose_math::rotate(turn, gripped, butt);
  float pivot[3];
  for (int axis = 0; axis < 3; ++axis) {
    pivot[axis] = hand[axis] - butt[axis];
  }
  entity::place_in_world(handle_, pivot, turn);
  float tip[3];
  pose_math::rotate(turn, tip_, tip);
  for (int axis = 0; axis < 3; ++axis) {
    tip[axis] += pivot[axis];
  }
  // The handle drawn from its butt to its tip, straight.
  float butt_end[3];
  pose_math::rotate(turn, butt_, butt_end);
  std::vector<float> handle_points(9);
  for (int axis = 0; axis < 3; ++axis) {
    butt_end[axis] += pivot[axis];
    handle_points[std::size_t(axis)] = butt_end[axis];
    handle_points[std::size_t(3 + axis)] = 0.5f * (butt_end[axis] + tip[axis]);
    handle_points[std::size_t(6 + axis)] = tip[axis];
  }
  handle_rope_.hold(butt_end);
  handle_rope_.set_points(handle_points);
  lash_[0].hold(tip);
  // The lash: our tapered lash moved on by the time since the last frame, from the handle's tip
  // along its long side, then drawn by the game's rope.
  float along[3];
  pose_math::rotate(turn, axis_, along);
  // The handle's up side (a posed lash's curl bends toward it): the handle's next axis after its
  // long side, turned with it.
  float across[3] = {0, 0, 0};
  for (int axis = 0; axis < 3; ++axis) {
    if (axis_[axis] != 0.0f) {
      across[(axis + 1) % 3] = 1.0f;
    }
  }
  float up[3];
  pose_math::rotate(turn, across, up);
  for (int axis = 0; axis < 3; ++axis) {
    along_[axis] = along[axis];
    up_[axis] = up[axis];
  }
  framed_ = true;
  const auto now = std::chrono::steady_clock::now();
  const float seconds = moved_at_ ? std::chrono::duration<float>(now - *moved_at_).count() : 0.0f;
  moved_at_ = now;
  const RopePhysics physics = own_physics_ ? *own_physics_ : rope_physics();
  WhipLashSettings settings;
  settings.gravity = physics.gravity;
  // The lash's own drag (tuned offline: 4 a second at the studio's damping 0.1) and the rope's
  // damping and air drag on top.
  settings.drag = 3.0f + 4.0f * physics.damping + 5.0f * physics.air_resistance;
  settings.ground = physics.collide != 0;
  settings.shape = physics.shape;
  settings.curl = physics.curl;
  settings.sway = physics.sway;
  // Ignore characters off (collide 1): the holder's body and the others near in the lash's way.
  if (physics.collide == 1 && holder_) {
    bodies_ = bodies_of(holder_, holder_left_, hand);
    settings.bodies = &bodies_;
  }
  if (bodies_logged_ != (settings.bodies ? int(bodies_.size()) : 0)) {
    bodies_logged_ = settings.bodies ? int(bodies_.size()) : 0;
    game_client::log("WHIP: %d body capsules in the lash's way", bodies_logged_);
  }
  // An animation's keys shape the lash (Lash to keys): exactly that shape, no physics; in the
  // body's frame (forward, right, up) when the animation keeps it so.
  if (!lash_shape_.empty() && lash_in_body_) {
    const float ahead[3] = {0.0f, 1.0f, 0.0f};
    const float upward[3] = {0.0f, 0.0f, 1.0f};
    float forward[3];
    float body_up[3];
    pose_math::rotate(lash_body_, ahead, forward);
    pose_math::rotate(lash_body_, upward, body_up);
    model_.shaped(tip, forward, body_up, out_of_handle(lash_shape_, along, forward, body_up));
  } else if (!lash_shape_.empty()) {
    model_.shaped(tip, along, up, lash_shape_);
  } else {
    model_.step(tip, along, up, seconds, ground_, settings);
  }
  if (settings.bodies) {
    log_inside(model_.points(), bodies_);
  }
  // Each part over its share of the points (the parts share the points where they meet).
  const std::vector<float>& points = model_.points();
  for (int part = 0; part < 3; ++part) {
    const auto first = points.begin() + std::ptrdiff_t(part_start_[part]) * 3;
    const auto last = points.begin() + std::ptrdiff_t(part_start_[part + 1] + 1) * 3;
    const std::vector<float> share(first, last);
    if (part > 0) {
      lash_[part].hold(share.data());
    }
    const bool set = lash_[part].set_points(share);
    if (set != part_drawn_[part]) {
      part_drawn_[part] = set;
      game_client::log("WHIP %s: lash part %d %s (%zu points, the rope has %zu)", name_.c_str(),
                       part, set ? "follows the lash" : "did not take the lash's points",
                       share.size() / 3, lash_[part].points().size() / 3);
    }
  }
}

void WhipProp::remove() {
  for (RopeProp& part : lash_) {
    part.remove();
  }
  handle_rope_.remove();
  if (handle_) {
    entity::remove(name_);
    handle_ = nullptr;
  }
}

}  // namespace kcds::runtime::physical_props
