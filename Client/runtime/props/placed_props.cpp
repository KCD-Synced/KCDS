#include "placed_props.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include "model_collision.hpp"
#include "prop_entity.hpp"
#include "prop_physics.hpp"
#include "../prefabs/game_prefabs.hpp"

namespace kcds::runtime::props::placed {
namespace {

using kcd2s::integration::PropKind;
using kcd2s::integration::PropOrder;
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
  // Frames until the simulated body's mass is checked; 0 once it was.
  unsigned mass_check_frames = 0;
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
  prop.moving = false;
  remember_report(prop);
}

// A simulated body whose mesh's collision carries no mass is built static instead; true when it
// stays rigid.
bool check_mass(std::uint32_t id, void* entity, Prop& prop) {
  if (prop.mass_check_frames == 0 || --prop.mass_check_frames > 0) {
    return true;
  }
  if (physics::mass(physics::body_of(entity)) > 0.0f) {
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

// Removes the prop's entity when it has one, then builds it from what the prop shows.
bool build(std::uint32_t id, Prop& prop) {
  const std::string name = entity_name(id);
  // A game prefab (a house) is built by the game's own spawner, solid where it
  // stands; it hangs on no body and is never simulated.
  const std::string prefab = prefabs::template_of(prop.model);
  if (!prefab.empty()) {
    prefabs::remove(name);
    return prefabs::spawn(name, prefab, prop.position, prop.rotation);
  }
  entity::remove(name);
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
  // A glide starts from where the prop stands now (a glide still running included).
  const bool glides =
      order.glide_ms > 0 && !simulated(prop) && prefabs::template_of(prop.model).empty();
  if (glides) {
    void* standing = entity::find(entity_name(order.id));
    float at[3]{};
    float turn[4]{};
    if (standing && prop.body == 0 && prop.glide_seconds > 0.0) {
      entity::world_pose(standing, at, turn);
    } else {
      std::copy(std::begin(prop.position), std::end(prop.position), at);
      std::copy(std::begin(prop.rotation), std::end(prop.rotation), turn);
    }
    std::copy(std::begin(at), std::end(at), prop.glide_from);
    std::copy(std::begin(turn), std::end(turn), prop.glide_from_turn);
    prop.glide_start = -1.0;
    prop.glide_seconds = order.glide_ms / 1000.0;
    prop.glide_arc = order.glide_arc;
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
  props.erase(order.id);
  waiting.erase(order.id);
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

// Every frame: the props that glide (Prop.glideTo) a step further, eased in and out, on their arc,
// turning on the way (rotations x y z w, the shorter way round).
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
    const double t = std::min(1.0, (now - prop.glide_start) / prop.glide_seconds);
    const float e = float(t * t * (3.0 - 2.0 * t));
    float at[3]{};
    for (std::size_t i = 0; i < 3; ++i) {
      at[i] = prop.glide_from[i] + (prop.position[i] - prop.glide_from[i]) * e;
    }
    at[2] += prop.glide_arc * float(std::sin(3.14159265358979 * t));
    float dot = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
      dot += prop.glide_from_turn[i] * prop.rotation[i];
    }
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    float turn[4]{};
    float length = 0.0f;
    for (std::size_t i = 0; i < 4; ++i) {
      turn[i] = prop.glide_from_turn[i] + (sign * prop.rotation[i] - prop.glide_from_turn[i]) * e;
      length += turn[i] * turn[i];
    }
    length = std::sqrt(length);
    for (std::size_t i = 0; i < 4 && length > 0.0f; ++i) {
      turn[i] /= length;
    }
    if (prop.body != 0) {
      entity::place_rotated(entity, at, turn);
    } else {
      entity::place_in_world(entity, at, turn);
    }
    if (t >= 1.0) {
      prop.glide_seconds = 0.0;
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
}

}  // namespace kcds::runtime::props::placed
