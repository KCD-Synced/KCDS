#include "placed_props.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "glide_path.hpp"
#include "model_collision.hpp"
#include "prop_entity.hpp"
#include "prop_ledges.hpp"
#include "prop_physics.hpp"
#include "../lanterns/lantern_light.hpp"
#include "../prefabs/game_prefabs.hpp"

namespace kcds::runtime::props::placed {
namespace {

using kcd2s::integration::PropEase;
using kcd2s::integration::PropKind;
using kcd2s::integration::PropOrder;
using kcd2s::integration::PropPathKey;
using kcd2s::integration::PropPhysics;
using kcd2s::integration::PropPush;
using kcd2s::integration::PropReport;

// A rigid prop of mass 0 takes the mass its mesh's collision gives it (the game's -1).
constexpr float kMeshMass = -1.0f;
// An area's collision wall is built as a static prop, then drawn with the game's
// editor material that draws nothing, so only its physics is there (spawned with that material
// the mesh came without physics).
constexpr const char* kWallMaterial = "objects/editor/nodraw";
// A simulated body's reports: at most this often while it moves, and only when it
// moved or turned this much since the last one.
constexpr double kReportSeconds = 0.1;
constexpr float kReportMeters = 0.01f;
constexpr float kReportTurn = 0.0005f;
// The frames after a rigid body is built before its mass is checked.
constexpr unsigned kMassCheckFrames = 10;
// A rigid body whose mesh carries no mass gets a box of its size: at least this thick on each side
// (m), and without a mass of the prop's, as heavy as that much of wood (kg/m^3), at least
// kLeastBoxMass (kg).
constexpr float kLeastMassBoxHalf = 0.01f;
constexpr float kMassBoxDensity = 700.0f;
constexpr float kLeastBoxMass = 0.05f;
// How often a body without mass gets kMassCheckFrames more for its box before it is built static.
constexpr unsigned kMassBoxTries = 3;
// The frames a simulated body sleeps before it is reported at rest.
constexpr unsigned kRestFrames = 3;
// A hit: a simulated body that moved at kHitSpeed (m/s) or faster and whose velocity
// (as the game's physics holds it) changed by kHitChange or more in one frame while it slowed
// down struck something: gravity alone changes it by well under 1 m/s a frame, and a glancing hit
// on a slope keeps most of the speed. Told at once with the speed it had, at most every
// kHitSeconds per prop.
constexpr float kHitSpeed = 2.0f;
constexpr float kHitChange = 2.0f;
constexpr double kHitSeconds = 0.3;
// Props on bodies this game does not show, and bodies rebuilt, are looked at this often (s).
constexpr double kBodyCheckSeconds = 0.5;
// A rigid body another game simulates comes as poses about every kReportSeconds: each is glided
// to over the time since the one before, within these bounds (s), so it moves on evenly a pose
// behind. One farther than kFollowJumpMeters (reset, teleported) is there at once.
constexpr double kFollowShortest = 0.05;
constexpr double kFollowLongest = 0.25;
constexpr float kFollowJumpMeters = 5.0f;

// SChildAttachParams: no flags attaches to the parent entity itself, flag 4 to a joint of its
// character by name (as the lights and the bursts on bodies).
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
constexpr std::int32_t kAttachToJoint = 4;
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);

struct Prop {
  std::string model;
  float position[3]{};
  float rotation[4]{};
  float scale[3]{};
  PropPhysics physics = PropPhysics::Static;
  float mass = 0.0f;
  // Whether this game simulates the rigid body (else it follows the poses the server passes on).
  bool simulate = false;
  // Frames until the simulated body's mass is checked; 0 once it was. Whether its body got a box
  // for the mass its mesh does not carry (add_mass_box).
  unsigned mass_check_frames = 0;
  bool mass_box = false;
  unsigned mass_box_tries = 0;
  // The simulated body moved since it last came to rest, and where this game last reported it.
  bool moving = false;
  unsigned asleep_frames = 0;
  double reported_at = 0.0;
  float reported_position[3]{};
  float reported_rotation[4]{};
  // The simulated body's velocity in the last frame and when it last struck something.
  std::array<float, 3> velocity{};
  double hit_at = -1.0;
  // The body it hangs on (a network id, 0 for none), the joint of it and the body's entity it was
  // hung on.
  std::uint32_t body = 0;
  std::string joint;
  void* parent = nullptr;
  // A glide the server asked for (Prop.glideTo, a thrown card, a hopping piece):
  // from where it stood to `position` over glide_seconds, lifted on an arc glide_arc metres high
  // at the middle; glide_seconds 0 when none runs. It starts at the first frame after the order.
  float glide_from[3]{};
  float glide_from_turn[4]{};
  double glide_start = -1.0;
  double glide_seconds = 0.0;
  float glide_arc = 0.0f;
  // Its pace, its spin (radians a second about the world's axes) and the poses it passes
  // (Prop.followPath; empty: straight to `position`).
  PropEase glide_ease = PropEase::Smooth;
  std::array<float, 3> glide_spin{};
  std::vector<PropPathKey> glide_path;
  // A rigid body another game simulates: when its last pose came (-1 before the first).
  double followed_at = -1.0;
};

Logger log_line = nullptr;
ReportFn report_fn = nullptr;
BodyOf body_of_fn = nullptr;
void* attach_child_fn = nullptr;
// Props on bodies this game does not show yet, by id, and when the bodies were last looked at.
std::unordered_map<std::uint32_t, PropOrder> waiting;
double body_checked_at = 0.0;
// Engine thread only: server prop id -> what it shows.
std::unordered_map<std::uint32_t, Prop> props;
// The time of the last frame (s), for the orders that come between frames.
double frame_now = 0.0;

std::string entity_name(std::uint32_t id) {
  return "kcds_prop_" + std::to_string(id);
}

bool simulated(const Prop& prop) {
  return prop.physics == PropPhysics::Rigid && prop.simulate;
}

entity::Physics body_of(const Prop& prop) {
  switch (prop.physics) {
    case PropPhysics::Static:
    case PropPhysics::Wall:
      return entity::Physics::Static;
    case PropPhysics::Rigid:
      // Only the game simulating it moves the body; every other one shows it where it is told.
      return prop.simulate ? entity::Physics::Rigid : entity::Physics::Static;
    default:
      return entity::Physics::None;
  }
}

void remember_report(Prop& prop) {
  for (std::size_t i = 0; i < 3; ++i) {
    prop.reported_position[i] = prop.position[i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    prop.reported_rotation[i] = prop.rotation[i];
  }
}

// Gives the prop's entity its body. A rigid one's mass is checked kMassCheckFrames later, once
// the physics took the body (right after physicalizing it reads 0).
// The larger models (an oven, a shed, a well) come without their physics proxies, and a model the
// game is still streaming in has none yet; a static body without any takes them from the model's
// file instead.
void add_missing_collision(void* entity, const Prop& prop) {
  void* physical = physics::body_of(entity);
  if (physical && !physics::has_parts(physical)) {
    model_collision::add(physical, prop.model, prop.scale[0]);
  }
}

void give_body(void* entity, Prop& prop) {
  entity::physicalize(entity, body_of(prop), prop.mass > 0.0f ? prop.mass : kMeshMass);
  if (body_of(prop) == entity::Physics::Static) {
    add_missing_collision(entity, prop);
  }
  prop.mass_check_frames = simulated(prop) ? kMassCheckFrames : 0;
  prop.mass_box = false;
  prop.mass_box_tries = 0;
  prop.moving = false;
  remember_report(prop);
}

// A simulated body whose mesh's collision carries no mass (the game's item meshes: a stone, an
// apple collide as drawn, with no density) is given a box of the mesh's size carrying the prop's
// mass, or one of its size when it has none; true when that holds it.
bool add_mass_box(std::uint32_t id, void* entity, const Prop& prop) {
  float min[3]{};
  float max[3]{};
  void* body = physics::body_of(entity);
  if (!body || !entity::local_bounds(entity, min, max)) {
    if (log_line) {
      log_line("PROP %u: no box for its mass (%s)", id, body ? "no bounds" : "no body");
    }
    return false;
  }
  float center[3]{};
  float half[3]{};
  for (std::size_t i = 0; i < 3; ++i) {
    center[i] = (min[i] + max[i]) * 0.5f * prop.scale[i];
    half[i] = std::max(kLeastMassBoxHalf, (max[i] - min[i]) * 0.5f * prop.scale[i]);
  }
  const float volume = 8.0f * half[0] * half[1] * half[2];
  const float mass =
      prop.mass > 0.0f ? prop.mass : std::max(kLeastBoxMass, volume * kMassBoxDensity);
  // The physics takes the new part on its next step (the game queues a change made while it
  // steps), so the mass is checked again a few frames later.
  if (!model_collision::add_box(body, center, half, mass)) {
    if (log_line) {
      log_line("PROP %u: its box for a mass of %.2f kg was not taken", id, mass);
    }
    return false;
  }
  if (log_line) {
    log_line(
        "PROP %u %s: its collision carries no mass; a box %.2f x %.2f x %.2f m carries %.2f kg", id,
        prop.model.c_str(), half[0] * 2.0f, half[1] * 2.0f, half[2] * 2.0f, mass);
  }
  return true;
}

// A simulated body whose mesh's collision carries no mass is given one (add_mass_box) and looked
// at again kMassCheckFrames later; one still without mass is built static instead. True while it
// stays rigid.
bool check_mass(std::uint32_t id, void* entity, Prop& prop) {
  if (prop.mass_check_frames == 0 || --prop.mass_check_frames > 0) {
    return true;
  }
  if (physics::mass(physics::body_of(entity)) > 0.0f) {
    return true;
  }
  // A box not taken yet (a body just built may refuse it) is tried again, and one taken is given
  // a few more frames, kMassBoxTries times before the body is built static.
  if (prop.mass_box_tries < kMassBoxTries) {
    ++prop.mass_box_tries;
    if (!prop.mass_box) {
      prop.mass_box = add_mass_box(id, entity, prop);
    }
    prop.mass_check_frames = kMassCheckFrames;
    return true;
  }
  if (log_line) {
    log_line("PROP %u %s: its collision carries no mass; give it a mass to build it rigid", id,
             prop.model.c_str());
  }
  prop.simulate = false;
  entity::physicalize(entity, entity::Physics::Static, kMeshMass);
  add_missing_collision(entity, prop);
  return false;
}

// Hangs the prop's entity on its body (on the joint when the body has one of that name) at its
// offset; false when this game has no such body.
bool hang(void* entity, Prop& prop) {
  prop.parent = body_of_fn ? body_of_fn(prop.body) : nullptr;
  if (!prop.parent || !attach_child_fn) {
    return false;
  }
  // A biped name ("Bip01 Head") finds the game's own joint ("Head").
  const std::string joint = entity::body_joint_name(prop.parent, prop.joint);
  const bool on_joint = !joint.empty();
  const AttachParams params{on_joint ? kAttachToJoint : 0, on_joint ? joint.c_str() : nullptr};
  reinterpret_cast<AttachChild>(attach_child_fn)(prop.parent, entity, &params);
  entity::place_rotated(entity, prop.position, prop.rotation);
  return true;
}

// The highest a fence's ledge stands over its foot (metres).
constexpr float kMostLedgeHeight = 0.9f;

// A fence of the game's (objects/.../fences/), not a gate or a post: vaulted as the level's are.
bool is_fence(const std::string& model) {
  std::string lower = model;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return lower.find("/fences/") != std::string::npos && lower.find("gate") == std::string::npos &&
         lower.find("post") == std::string::npos && lower.find("_end") == std::string::npos;
}

// A standing fence prop gets a vaultable ledge along the top of its model's box, along its longer
// side (props/prop_ledges.hpp); anything else has none.
void fit_ledge(std::uint32_t id, const Prop& prop, void* entity) {
  float low[3];
  float high[3];
  const bool standing = prop.body == 0 && prop.physics != PropPhysics::None && !simulated(prop);
  if (!entity || !standing || !is_fence(prop.model) || !entity::local_bounds(entity, low, high)) {
    ledges::remove(id);
    return;
  }
  const bool along_x = high[0] - low[0] >= high[1] - low[1];
  const float middle_x = (low[0] + high[0]) * 0.5f;
  const float middle_y = (low[1] + high[1]) * 0.5f;
  // The player's ledge search looks from about the waist and loses a ledge high over it close
  // to the fence (a 1.1 m stick fence's top was found only from a step away): the ledge is at
  // most kMostLedgeHeight over the fence's foot, as the level's own fence ledges are.
  const float top = (std::min)(high[2], low[2] + kMostLedgeHeight);
  const float start[3] = {along_x ? low[0] : middle_x, along_x ? middle_y : low[1], top};
  const float end[3] = {along_x ? high[0] : middle_x, along_x ? middle_y : high[1], top};
  float from[3];
  float to[3];
  entity::world_point(entity, start, from);
  entity::world_point(entity, end, to);
  ledges::set(id, from, to);
}

// Removes the prop's entity when it has one, then builds it from what the prop shows.
bool build(std::uint32_t id, Prop& prop) {
  ledges::remove(id);
  const std::string name = entity_name(id);
  // A game prefab (a house) is built by the game's own spawner, solid where it
  // stands; it hangs on no body and is never simulated.
  const std::string prefab = prefabs::template_of(prop.model);
  if (!prefab.empty()) {
    prefabs::remove(name);
    return prefabs::spawn(name, prefab, prop.position, prop.rotation);
  }
  entity::remove(name);
  lanterns::light::put_out(id);
  if (prop.body != 0) {
    // On a body: the mesh without physics, a child of the body.
    void* hanging = entity::spawn_mesh(name, prop.position, prop.model, std::string());
    if (!hanging) {
      return false;
    }
    entity::set_scale(hanging, prop.scale);
    // No physics proxy at all: one only switched off came back when its body's physics did, after
    // NoClip, and the mesh's collider threw its wearer across the map.
    entity::physicalize(hanging, entity::Physics::None, 0.0f);
    if (!hang(hanging, prop)) {
      entity::remove(name);
      return false;
    }
    // A carried lamp is lit with the game's own lantern light (lanterns/lantern_light.hpp).
    if (lanterns::light::is_lantern(prop.model)) {
      const bool lit = lanterns::light::light(id, hanging, attach_child_fn);
      if (log_line) {
        log_line("PROP %u lantern %s", id, lit ? "lit with the game's lantern light" : "not lit");
      }
    }
    return true;
  }
  // Physics 'none' spawns the mesh with its physics off: the BasicEntity physicalizes itself as
  // it spawns, and that body stayed, a wide invisible obstacle around the prop.
  void* body = nullptr;
  if (prop.physics == PropPhysics::None) {
    body = entity::spawn_mesh(name, prop.position, prop.model, std::string());
  } else {
    body = entity::spawn_solid_mesh(name, prop.position, prop.model);
  }
  if (!body) {
    return false;
  }
  entity::rotate(body, prop.rotation);
  entity::set_scale(body, prop.scale);
  give_body(body, prop);
  if (prop.physics == PropPhysics::Wall) {
    entity::draw_with_material(body, kWallMaterial);
  }
  fit_ledge(id, prop, body);
  return true;
}

void spawn(const PropOrder& order) {
  Prop prop;
  prop.model = order.model;
  for (std::size_t i = 0; i < 3; ++i) {
    prop.position[i] = order.position[i];
    prop.scale[i] = order.scale[i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    prop.rotation[i] = order.rotation[i];
  }
  prop.physics = order.physics;
  prop.mass = order.mass;
  prop.body = order.body;
  prop.joint = order.joint;
  waiting.erase(order.id);
  if (prop.body != 0 && !(body_of_fn && body_of_fn(prop.body))) {
    // The body is not in this game (yet): the prop comes when it does.
    waiting[order.id] = order;
    entity::remove(entity_name(order.id));
    lanterns::light::put_out(order.id);
    props.erase(order.id);
    return;
  }
  const bool built = build(order.id, prop);
  if (log_line) {
    log_line("PROP %u %s: %s", order.id, order.model.c_str(), built ? "spawned" : "not spawned");
  }
  if (built) {
    props[order.id] = prop;
  }
}

void update(const PropOrder& order) {
  const auto found = props.find(order.id);
  if (found == props.end()) {
    return;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    found->second.scale[i] = order.scale[i];
  }
  found->second.physics = order.physics;
  found->second.mass = order.mass;
  if (!build(order.id, found->second)) {
    props.erase(found);
  }
}

// Moves a prop as it is: its entity keeps its mesh, size and collision. A body this
// game simulates comes to rest there.
void move(const PropOrder& order) {
  const auto found = props.find(order.id);
  if (found == props.end()) {
    return;
  }
  Prop& prop = found->second;
  const bool can_glide = !simulated(prop) && prefabs::template_of(prop.model).empty();
  const bool ordered = order.glide_ms > 0 && can_glide;
  // A glide starts from where the prop stands now (a glide still running included).
  void* standing = entity::find(entity_name(order.id));
  float at[3]{};
  float turn[4]{};
  if (standing && prop.body == 0 && prop.glide_seconds > 0.0) {
    entity::world_pose(standing, at, turn);
  } else {
    std::copy(std::begin(prop.position), std::end(prop.position), at);
    std::copy(std::begin(prop.rotation), std::end(prop.rotation), turn);
  }
  // A rigid body another game simulates glides from pose to pose at an even pace.
  bool follows = false;
  double follow_seconds = kReportSeconds;
  if (!ordered && can_glide && prop.physics == PropPhysics::Rigid && prop.body == 0) {
    const float dx = order.position[0] - at[0];
    const float dy = order.position[1] - at[1];
    const float dz = order.position[2] - at[2];
    follows = std::sqrt(dx * dx + dy * dy + dz * dz) <= kFollowJumpMeters;
    if (prop.followed_at >= 0.0) {
      follow_seconds = std::clamp(frame_now - prop.followed_at, kFollowShortest, kFollowLongest);
    }
    prop.followed_at = frame_now;
  }
  const bool glides = ordered || follows;
  if (glides) {
    std::copy(std::begin(at), std::end(at), prop.glide_from);
    std::copy(std::begin(turn), std::end(turn), prop.glide_from_turn);
    prop.glide_start = -1.0;
    prop.glide_seconds = ordered ? order.glide_ms / 1000.0 : follow_seconds;
    prop.glide_arc = ordered ? order.glide_arc : 0.0f;
    prop.glide_ease = ordered ? order.glide_ease : PropEase::Linear;
    prop.glide_spin = ordered ? order.glide_spin : std::array<float, 3>{};
    prop.glide_path = ordered ? order.path : std::vector<PropPathKey>{};
  } else {
    prop.glide_seconds = 0.0;
  }
  for (std::size_t i = 0; i < 3; ++i) {
    prop.position[i] = order.position[i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    prop.rotation[i] = order.rotation[i];
  }
  if (glides) {
    // frame() moves it from the next frame on.
    return;
  }
  if (!prefabs::template_of(prop.model).empty()) {
    // The game lays a prefab's brushes once where its entity stands: it is built anew there.
    if (!build(order.id, prop)) {
      props.erase(found);
    }
    return;
  }
  void* entity = entity::find(entity_name(order.id));
  if (!entity) {
    return;
  }
  if (prop.body != 0) {
    // On a body the pose is the offset from it.
    entity::place_rotated(entity, prop.position, prop.rotation);
    return;
  }
  entity::place_in_world(entity, prop.position, prop.rotation);
  fit_ledge(order.id, prop, entity);
  if (simulated(prop)) {
    void* body = physics::body_of(entity);
    physics::set_velocity(body, {}, {});
    physics::set_awake(body, false);
    prop.moving = false;
    remember_report(prop);
  }
}

// This game simulates the rigid body from now on, or follows the server's poses.
void simulate(const PropOrder& order) {
  const auto found = props.find(order.id);
  if (found == props.end() || found->second.simulate == order.simulate) {
    return;
  }
  Prop& prop = found->second;
  prop.simulate = order.simulate;
  // The body is built anew where the prop is: a body once given cannot change its kind.
  void* entity = entity::find(entity_name(order.id));
  if (entity && prop.physics == PropPhysics::Rigid) {
    entity::world_pose(entity, prop.position, prop.rotation);
    if (!build(order.id, prop)) {
      props.erase(found);
      return;
    }
  }
  if (log_line) {
    log_line("PROP %u: %s", order.id,
             simulated(prop) ? "this game simulates its body" : "its body follows the server");
  }
}

void push(const PropOrder& order) {
  const auto found = props.find(order.id);
  if (found == props.end() || !simulated(found->second)) {
    return;
  }
  void* body = physics::body_of(entity::find(entity_name(order.id)));
  if (!body) {
    return;
  }
  switch (order.push) {
    case PropPush::Impulse:
      physics::push(body, order.linear, order.angular,
                    order.has_point ? order.point.data() : nullptr);
      break;
    case PropPush::Velocity:
      physics::set_velocity(body, order.linear, order.angular);
      break;
    case PropPush::Wake:
      physics::set_awake(body, true);
      break;
    case PropPush::Sleep:
      physics::set_awake(body, false);
      break;
  }
}

void destroy(const PropOrder& order) {
  const auto found = props.find(order.id);
  if (found != props.end() && !prefabs::template_of(found->second.model).empty()) {
    prefabs::remove(entity_name(order.id));
  }
  entity::remove(entity_name(order.id));
  lanterns::light::put_out(order.id);
  props.erase(order.id);
  waiting.erase(order.id);
  ledges::remove(order.id);
}

// Props on bodies: one whose body came is shown, one whose body went or was rebuilt (a replica
// made again) is hung again.
void keep_hanging(double now) {
  if (now - body_checked_at < kBodyCheckSeconds) {
    return;
  }
  body_checked_at = now;
  std::vector<PropOrder> arrived;
  for (const auto& [id, order] : waiting) {
    if (body_of_fn && body_of_fn(order.body)) {
      arrived.push_back(order);
    }
  }
  for (const PropOrder& order : arrived) {
    spawn(order);
  }
  std::vector<std::uint32_t> gone;
  for (auto& [id, prop] : props) {
    if (prop.body == 0) {
      continue;
    }
    void* now_body = body_of_fn ? body_of_fn(prop.body) : nullptr;
    if (now_body == prop.parent) {
      continue;
    }
    if (!now_body || !build(id, prop)) {
      gone.push_back(id);
    }
  }
  for (const std::uint32_t id : gone) {
    // Its body left this game: shown again when it comes back.
    auto found = props.find(id);
    PropOrder order;
    order.id = id;
    order.model = found->second.model;
    for (std::size_t i = 0; i < 3; ++i) {
      order.position[i] = found->second.position[i];
      order.scale[i] = found->second.scale[i];
    }
    for (std::size_t i = 0; i < 4; ++i) {
      order.rotation[i] = found->second.rotation[i];
    }
    order.physics = found->second.physics;
    order.body = found->second.body;
    order.joint = found->second.joint;
    entity::remove(entity_name(id));
    lanterns::light::put_out(id);
    props.erase(found);
    waiting[id] = order;
  }
}

float length(const std::array<float, 3>& v) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

float distance(const float a[3], const float b[3]) {
  const float x = a[0] - b[0];
  const float y = a[1] - b[1];
  const float z = a[2] - b[2];
  return std::sqrt(x * x + y * y + z * z);
}

// 1 - |dot|: 0 for the same turn.
float turn(const float a[4], const float b[4]) {
  const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
  return 1.0f - std::fabs(dot);
}

void report(std::uint32_t id, Prop& prop, bool resting, double now, float hit_speed = 0.0f) {
  PropReport message;
  message.id = id;
  for (std::size_t i = 0; i < 3; ++i) {
    message.position[i] = prop.position[i];
  }
  for (std::size_t i = 0; i < 4; ++i) {
    message.rotation[i] = prop.rotation[i];
  }
  message.resting = resting;
  message.hit_speed = hit_speed;
  if (report_fn && report_fn(message)) {
    prop.reported_at = now;
    remember_report(prop);
  }
}

// The glide a prop runs: its path's keys, or the one key of where the order put it.
glide::Glide glide_of(const Prop& prop) {
  glide::Glide out;
  std::copy(std::begin(prop.glide_from), std::end(prop.glide_from), out.from);
  std::copy(std::begin(prop.glide_from_turn), std::end(prop.glide_from_turn), out.from_turn);
  out.ease = prop.glide_ease;
  std::copy(prop.glide_spin.begin(), prop.glide_spin.end(), out.spin);
  if (!prop.glide_path.empty()) {
    out.keys = prop.glide_path;
    return out;
  }
  PropPathKey end;
  end.ms = std::uint32_t(std::max(1.0, prop.glide_seconds * 1000.0));
  std::copy(std::begin(prop.position), std::end(prop.position), end.position.begin());
  std::copy(std::begin(prop.rotation), std::end(prop.rotation), end.rotation.begin());
  end.arc = prop.glide_arc;
  out.keys.push_back(end);
  return out;
}

// Every frame: the props that glide (Prop.glideTo, Prop.followPath, a rigid body another game
// simulates) a step further along their glide; at its end they stand where the order put them.
void glide(double now) {
  for (auto& [id, prop] : props) {
    if (prop.glide_seconds <= 0.0) {
      continue;
    }
    if (prop.glide_start < 0.0) {
      prop.glide_start = now;
    }
    void* entity = entity::find(entity_name(id));
    if (!entity) {
      prop.glide_seconds = 0.0;
      continue;
    }
    float at[3]{};
    float turn[4]{};
    const bool runs = glide::pose_at(glide_of(prop), now - prop.glide_start, at, turn);
    if (!runs) {
      std::copy(std::begin(prop.position), std::end(prop.position), at);
      std::copy(std::begin(prop.rotation), std::end(prop.rotation), turn);
      prop.glide_seconds = 0.0;
      prop.glide_path.clear();
    }
    if (prop.body != 0) {
      entity::place_rotated(entity, at, turn);
    } else {
      entity::place_in_world(entity, at, turn);
    }
  }
}

}  // namespace

void install(Logger logger, ReportFn report, BodyOf body_of, void* attach_child) {
  log_line = logger;
  report_fn = report;
  body_of_fn = body_of;
  attach_child_fn = attach_child;
}

void apply(const PropOrder& order) {
  switch (order.kind) {
    case PropKind::Spawn:
      spawn(order);
      break;
    case PropKind::Update:
      update(order);
      break;
    case PropKind::Destroy:
      destroy(order);
      break;
    case PropKind::Move:
      move(order);
      break;
    case PropKind::Simulate:
      simulate(order);
      break;
    case PropKind::Push:
      push(order);
      break;
  }
}

void frame(double now) {
  keep_hanging(now);
  glide(now);
  ledges::frame();
  for (auto& [id, prop] : props) {
    if (!simulated(prop)) {
      prop.velocity = {};
      continue;
    }
    void* entity = entity::find(entity_name(id));
    if (!entity || !check_mass(id, entity, prop)) {
      continue;
    }
    entity::world_pose(entity, prop.position, prop.rotation);
    // A hit: fast a frame ago, and since then slower with a sharply changed velocity.
    std::array<float, 3> velocity{};
    physics::velocity(physics::body_of(entity), velocity);
    const float before = length(prop.velocity);
    const std::array<float, 3> change = {velocity[0] - prop.velocity[0],
                                         velocity[1] - prop.velocity[1],
                                         velocity[2] - prop.velocity[2]};
    if (before >= kHitSpeed && length(velocity) < before && length(change) >= kHitChange &&
        (prop.hit_at < 0.0 || now - prop.hit_at >= kHitSeconds)) {
      prop.hit_at = now;
      report(id, prop, false, now, before);
      if (log_line) {
        log_line("PROP %u struck something at %.1f m/s", id, before);
      }
    }
    prop.velocity = velocity;
    const bool moved = distance(prop.position, prop.reported_position) >= kReportMeters ||
                       turn(prop.rotation, prop.reported_rotation) >= kReportTurn;
    if (physics::awake(physics::body_of(entity))) {
      prop.asleep_frames = 0;
      if (moved && now - prop.reported_at >= kReportSeconds) {
        prop.moving = true;
        report(id, prop, false, now);
      }
      continue;
    }
    // The entity shows the body's last steps a frame or two after it fell asleep.
    if (++prop.asleep_frames < kRestFrames) {
      continue;
    }
    if (prop.moving || moved) {
      prop.moving = false;
      report(id, prop, true, now);
      if (log_line) {
        log_line("PROP %u rests at %.2f %.2f %.2f", id, prop.position[0], prop.position[1],
                 prop.position[2]);
      }
    }
  }
}

void* entity_of(std::uint32_t id) {
  if (props.count(id) == 0) {
    return nullptr;
  }
  return entity::find(entity_name(id));
}

void reset() {
  props.clear();
  waiting.clear();
  body_checked_at = 0.0;
  ledges::reset();
}

}  // namespace kcds::runtime::props::placed
