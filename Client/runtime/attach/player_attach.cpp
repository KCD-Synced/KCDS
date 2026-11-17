#include "player_attach.hpp"

#include <intrin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <string>
#include <cstddef>
#include <cstring>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../camera/photo_mode.hpp"
#include "../props/prop_entity.hpp"
#include "../riding/passenger_pose.hpp"

namespace kcds::runtime::attach {
namespace {

// A body carried this high or higher above the body it hangs on sits on it (a horse's back), and
// holds on there once its climb is over (riding/passenger_pose.hpp); lower, it climbs down.
constexpr float kSeatedAbove = 0.5f;

// CEntity: SetWorldTM(matrix, reason), GetWorldTM; its physics component's EnablePhysics(bool).
constexpr std::size_t kSetWorldTM = 240;
constexpr std::size_t kGetWorldTM = 256;
constexpr std::size_t kEnablePhysics = 192;
constexpr double kPi = 3.14159265358979323846;
// The actions an attached player keeps: looking around, the menus and the photo mode (F1) with its
// own keys (Libs/Config/defaultProfile.xml, photomode_enable and photomode_base).
constexpr const char* kKeptPrefixes[] = {"rotate",          "menu",
                                         "pause",           "photomode",
                                         "take_screenshot", "toggle_hints_visibility",
                                         "toggle_mode",     "close_photomode"};
constexpr unsigned kReleased = 2;

using SetWorldTM = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);
using GetWorldTM = const float*(__fastcall*)(void* entity);
using EnablePhysics = void(__fastcall*)(void* component, bool enable);
struct SharedPointer {
  void* object;
  void* control;
};
using PhysicsComponent = SharedPointer*(__fastcall*)(void* entity, SharedPointer* out, int create);

Natives natives;
Logger log_line = nullptr;
// The order about the local player, and those about the others, with when each came (a moving
// order's way is counted from then).
using Clock = std::chrono::steady_clock;
kcd2s::integration::PlayerAttach order;
Clock::time_point order_since;
std::unordered_map<std::uint32_t, kcd2s::integration::PlayerAttach> others;
std::unordered_map<std::uint32_t, Clock::time_point> others_since;
// The body whose physics this took away, while attached.
void* held_body = nullptr;
// The replicas whose physics this took away: the player's id and the entity.
std::unordered_map<std::uint32_t, void*> held_replicas;

template <typename Function>
Function slot(void* object, std::size_t offset) {
  return *reinterpret_cast<Function*>(*static_cast<std::uint8_t**>(object) + offset);
}

void set_physics(void* entity, bool enable) {
  if (!natives.physics_component) {
    return;
  }
  SharedPointer component{};
  reinterpret_cast<PhysicsComponent>(natives.physics_component)(entity, &component, 1);
  if (!component.object) {
    return;
  }
  slot<EnablePhysics>(component.object, kEnablePhysics)(component.object, enable);
  // The getter hands out a reference: the control block's release, as std::shared_ptr's.
  if (component.control) {
    auto* counts =
        reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(component.control) + 8);
    if (_InterlockedDecrement(counts) == 0) {
      slot<void(__fastcall*)(void*)>(component.control, 0)(component.control);
      auto* weak =
          reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(component.control) + 12);
      if (_InterlockedDecrement(weak) == 0) {
        slot<void(__fastcall*)(void*)>(component.control, 8)(component.control);
      }
    }
  }
}

double elapsed_ms(Clock::time_point since) {
  return std::chrono::duration<double, std::milli>(Clock::now() - since).count();
}

// Whether an order's body sits on the body it is carried on, its climb over.
bool seated(const kcd2s::integration::PlayerAttach& given, double elapsed) {
  if ((given.flags & kcd2s::integration::kAttachToBody) == 0 || given.target == 0) {
    return false;
  }
  const bool moving =
      (given.flags & kcd2s::integration::kAttachMoving) != 0 && elapsed < double(given.moving_ms);
  return !moving && given.up >= kSeatedAbove;
}

void release_body() {
  if (!held_body) {
    return;
  }
  set_physics(held_body, true);
  held_body = nullptr;
  if (log_line) {
    log_line("ATTACH body released");
  }
}

// A matrix's place and its heading about the up axis (its rows, flat on the ground).
BodyPose flat_pose(const float* matrix) {
  BodyPose pose;
  pose.x = matrix[3];
  pose.y = matrix[7];
  pose.z = matrix[11];
  pose.heading = static_cast<float>(std::atan2(double(matrix[4]), double(matrix[0])));
  return pose;
}

// A seated passenger's seat: a bodyless entity of ours hanging from the joint the game's rider sits
// on (the horse's "rider" bone, which the horse's clips move with every step), placed where the
// passenger sits, and the passenger's body hanging from it (CEntity::AttachChild). The engine moves
// the horse, the bone, the seat and the body, and the player's camera with it, in one update, as it
// does the rider: placed every frame from the horse, the body lagged a frame behind it and the
// rider flew to and fro on the passenger's screen, and it sat still on a trotting horse. The body
// never hangs from the horse itself: the game then took the player for linked to the horse (the
// actor's linked object), asked the horse for its rider's sync and, once the rider had left,
// failed on it every frame (the game stood still).
constexpr char kRiderJoint[] = "rider";
constexpr char kSeatClass[] = "BasicEntity";
// AttachChild's parameters: the flags (keep the world place; hang from the parent character's
// joint) and the joint's name. CEntity DetachThis (slot 24) and GetParent (slot 27).
constexpr std::int32_t kKeepWorldPlace = 1;
constexpr std::int32_t kOnJoint = 4;
constexpr std::size_t kDetachThisSlot = 24;
constexpr std::size_t kGetParentSlot = 27;
// CEntity FreeSlot (slot 87) and the model slot a BasicEntity's script fills.
constexpr std::size_t kFreeSlotSlot = 87;
constexpr int kModelSlot = 0;
struct AttachParams {
  std::int32_t flags;
  const char* bone;
};
using AttachChild = bool(__fastcall*)(void* parent, void* child, const AttachParams* params);

struct Seat {
  std::string name;
  void* horse = nullptr;
  void* anchor = nullptr;
  void* body = nullptr;
};
// The local body's seat, and the replicas' by player.
Seat local_seat;
std::unordered_map<std::uint32_t, Seat> replica_seats;

template <typename Function>
Function entity_slot(void* entity, std::size_t index) {
  return (*reinterpret_cast<Function**>(entity))[index];
}

void* parent_of(void* entity) {
  return entity_slot<void*(__fastcall*)(void*)>(entity, kGetParentSlot)(entity);
}

void conjugate(const float q[4], float out[4]) {
  out[0] = -q[0];
  out[1] = -q[1];
  out[2] = -q[2];
  out[3] = q[3];
}

void quat_multiply(const float a[4], const float b[4], float out[4]) {
  out[0] = a[3] * b[0] + b[3] * a[0] + (a[1] * b[2] - a[2] * b[1]);
  out[1] = a[3] * b[1] + b[3] * a[1] + (a[2] * b[0] - a[0] * b[2]);
  out[2] = a[3] * b[2] + b[3] * a[2] + (a[0] * b[1] - a[1] * b[0]);
  out[3] = a[3] * b[3] - (a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

void quat_rotate(const float q[4], const float v[3], float out[3]) {
  const float p[4] = {v[0], v[1], v[2], 0.0f};
  float inverse[4];
  conjugate(q, inverse);
  float a[4];
  float b[4];
  quat_multiply(q, p, a);
  quat_multiply(a, inverse, b);
  out[0] = b[0];
  out[1] = b[1];
  out[2] = b[2];
}

// The body lets go of its seat where it is, and the seat goes.
void leave_seat(Seat& seat) {
  if (seat.body && seat.anchor && parent_of(seat.body) == seat.anchor) {
    entity_slot<void(__fastcall*)(void*, int, int)>(seat.body, kDetachThisSlot)(seat.body,
                                                                                kKeepWorldPlace, 0);
  }
  if (seat.anchor) {
    kcds::runtime::props::entity::remove(seat.name);
  }
  seat = Seat{};
}

// The body sits at `place` on the horse: the seat is made there on the rider's joint and the body
// hangs from it. False when the horse has no such joint or something failed (the body is then
// placed every frame instead).
bool take_seat(Seat& seat, const std::string& name, void* horse, void* body,
               const BodyPose& place) {
  if (!natives.attach_child) {
    return false;
  }
  float joint_position[3];
  float joint_rotation[4];
  if (!kcds::runtime::props::entity::joint_pose(horse, kRiderJoint, joint_position,
                                                joint_rotation)) {
    return false;
  }
  const float at[3] = {place.x, place.y, place.z};
  void* anchor = kcds::runtime::props::entity::spawn_class(name, kSeatClass, at);
  if (!anchor) {
    return false;
  }
  // The class's default model (a white pyramid) and the physics its script gives it go, as the
  // lights' carriers lose theirs (lights/lights.cpp): the seat is only a place.
  entity_slot<void(__fastcall*)(void*, int)>(anchor, kFreeSlotSlot)(anchor, kModelSlot);
  kcds::runtime::props::entity::physicalize(anchor, kcds::runtime::props::entity::Physics::None,
                                            0.0f);
  const auto attach = reinterpret_cast<AttachChild>(natives.attach_child);
  const AttachParams on_joint{kOnJoint, kRiderJoint};
  attach(horse, anchor, &on_joint);
  // Where the seat is from the joint: the joint's turn undone from the seat's place and heading.
  float joint_inverse[4];
  conjugate(joint_rotation, joint_inverse);
  const float apart[3] = {place.x - joint_position[0], place.y - joint_position[1],
                          place.z - joint_position[2]};
  float local_position[3];
  quat_rotate(joint_inverse, apart, local_position);
  const float heading[4] = {0.0f, 0.0f, std::sin(place.heading * 0.5f),
                            std::cos(place.heading * 0.5f)};
  float local_rotation[4];
  quat_multiply(joint_inverse, heading, local_rotation);
  kcds::runtime::props::entity::place_rotated(anchor, local_position, local_rotation);
  // The body exactly there, then hanging from the seat.
  const float c = std::cos(place.heading);
  const float s = std::sin(place.heading);
  const float matrix[12] = {c, -s, 0, place.x, s, c, 0, place.y, 0, 0, 1, place.z};
  slot<SetWorldTM>(body, kSetWorldTM)(body, matrix, 0);
  const AttachParams keep{kKeepWorldPlace, nullptr};
  attach(anchor, body, &keep);
  if (parent_of(body) != anchor) {
    kcds::runtime::props::entity::remove(name);
    return false;
  }
  seat.name = name;
  seat.horse = horse;
  seat.anchor = anchor;
  seat.body = body;
  return true;
}

// Where a horse is: its root's place and heading.
BodyPose horse_pose(void* horse) {
  return flat_pose(slot<GetWorldTM>(horse, kGetWorldTM)(horse));
}

// The rider's joints a passenger leans and holds on by: the lowest of the spine, at the waist,
// and the neck. The hands hold on that far to each side of it and a little behind.
constexpr char kRiderSpine[] = "Spine";
constexpr char kRiderNeck[] = "Neck";
constexpr float kHoldSide = 0.18f;
constexpr float kHoldBehind = 0.04f;

// The rider of the horse a passenger sits on, in the passenger's character space (the entity's
// axes at its origin, as the character sits in it): how their spine runs and where the
// passenger's hands hold on (riding/passenger_hold.hpp). Unknown without a rider or their joints.
kcds::runtime::riding::passenger_hold::Rider rider_seen(std::uint32_t horse, void* passenger) {
  kcds::runtime::riding::passenger_hold::Rider seen;
  void* rider = natives.rider_body ? natives.rider_body(horse) : nullptr;
  if (!rider || rider == passenger) {
    return seen;
  }
  float spine[3];
  float neck[3];
  float turn[4];
  if (!kcds::runtime::props::entity::joint_pose(rider, kRiderSpine, spine, turn) ||
      !kcds::runtime::props::entity::joint_pose(rider, kRiderNeck, neck, turn)) {
    return seen;
  }
  const float* m = slot<GetWorldTM>(passenger, kGetWorldTM)(passenger);
  const float* r = slot<GetWorldTM>(rider, kGetWorldTM)(rider);
  // A world direction in the passenger's axes (the matrix's columns: right, forward, up).
  const auto direction = [m](const float v[3], float out[3]) {
    out[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2];
    out[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2];
    out[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2];
  };
  const auto point = [m, &direction](const float p[3], float out[3]) {
    const float d[3] = {p[0] - m[3], p[1] - m[7], p[2] - m[11]};
    direction(d, out);
  };
  const float up_the_spine[3] = {neck[0] - spine[0], neck[1] - spine[1], neck[2] - spine[2]};
  direction(up_the_spine, seen.spine);
  const float right[3] = {r[0], r[4], r[8]};
  const float forward[3] = {r[1], r[5], r[9]};
  for (int side = 0; side < 2; ++side) {
    const float sign = side == 0 ? -1.0f : 1.0f;
    const float at[3] = {spine[0] + right[0] * sign * kHoldSide - forward[0] * kHoldBehind,
                         spine[1] + right[1] * sign * kHoldSide - forward[1] * kHoldBehind,
                         spine[2] + right[2] * sign * kHoldSide - forward[2] * kHoldBehind};
    point(at, seen.hands[side]);
  }
  seen.known = true;
  return seen;
}

}  // namespace

void install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
}

void apply_order(const kcd2s::integration::PlayerAttach& given) {
  if (given.flags & kcd2s::integration::kAttachIsLocalPlayer) {
    order = given;
    order_since = Clock::now();
  } else if (given.target == 0) {
    others.erase(given.player);
    others_since.erase(given.player);
  } else {
    others[given.player] = given;
    others_since[given.player] = Clock::now();
  }
}

void clear() {
  order = {};
  others.clear();
  others_since.clear();
  kcds::runtime::riding::passenger_pose::reset();
  // The world is going: its entities go with it.
  held_body = nullptr;
  held_replicas.clear();
  local_seat = Seat{};
  replica_seats.clear();
}

void let_go_of(void* entity) {
  if (!entity) {
    return;
  }
  if (local_seat.horse == entity) {
    leave_seat(local_seat);
  }
  for (auto it = replica_seats.begin(); it != replica_seats.end();) {
    if (it->second.horse == entity) {
      leave_seat(it->second);
      it = replica_seats.erase(it);
    } else {
      ++it;
    }
  }
}

bool seat_pose(std::uint32_t player, BodyPose& pose) {
  const auto found = replica_seats.find(player);
  if (found == replica_seats.end() || !found->second.anchor) {
    return false;
  }
  pose = flat_pose(slot<GetWorldTM>(found->second.anchor, kGetWorldTM)(found->second.anchor));
  return true;
}

namespace {

// The seats of the replicas: a seated passenger's body sits on one, any other leaves it.
void keep_replica_seats() {
  for (auto it = replica_seats.begin(); it != replica_seats.end();) {
    const auto found = others.find(it->first);
    void* entity = natives.replica_entity(it->first);
    void* horse = found != others.end() && natives.body_entity
                      ? natives.body_entity(found->second.target)
                      : nullptr;
    const auto since = others_since.find(it->first);
    const double elapsed = since == others_since.end() ? 1e9 : elapsed_ms(since->second);
    const bool keep = found != others.end() && seated(found->second, elapsed) &&
                      entity == it->second.body && horse == it->second.horse;
    if (keep) {
      ++it;
      continue;
    }
    // A body that is gone hangs from nothing any more.
    if (entity != it->second.body) {
      it->second.body = nullptr;
    }
    leave_seat(it->second);
    it = replica_seats.erase(it);
  }
  for (const auto& [player, attached] : others) {
    const auto since = others_since.find(player);
    const double elapsed = since == others_since.end() ? 1e9 : elapsed_ms(since->second);
    if (replica_seats.count(player) || !seated(attached, elapsed) || !natives.body_entity) {
      continue;
    }
    void* entity = natives.replica_entity(player);
    void* horse = natives.body_entity(attached.target);
    if (!entity || !horse) {
      continue;
    }
    const BodyPose place = beside(attached, horse_pose(horse));
    Seat seat;
    if (take_seat(seat, "kcds_seat_" + std::to_string(player), horse, entity, place)) {
      replica_seats[player] = seat;
      if (log_line) {
        log_line("ATTACH replica of player %u sits on horse %u", unsigned(player),
                 unsigned(attached.target));
      }
    }
  }
}

// The attached replicas without physics; one no longer attached gets it back.
void keep_replicas() {
  keep_replica_seats();
  for (auto it = held_replicas.begin(); it != held_replicas.end();) {
    void* entity = natives.replica_entity(it->first);
    if (others.count(it->first) && entity == it->second) {
      ++it;
      continue;
    }
    if (entity == it->second) {
      set_physics(entity, true);
    }
    it = held_replicas.erase(it);
  }
  for (const auto& [player, attached] : others) {
    const auto since = others_since.find(player);
    const double elapsed = since == others_since.end() ? 1e9 : elapsed_ms(since->second);
    // A horse's passenger sitting behind its rider holds on to them.
    if (seated(attached, elapsed)) {
      if (void* entity = natives.replica_entity(player)) {
        kcds::runtime::riding::passenger_pose::hold(entity, rider_seen(attached.target, entity));
      }
    }
    if (held_replicas.count(player)) {
      continue;
    }
    if (void* entity = natives.replica_entity(player)) {
      set_physics(entity, false);
      held_replicas[player] = entity;
      if (log_line) {
        log_line("ATTACH replica of player %u: physics off", unsigned(player));
      }
    }
  }
}

}  // namespace

const kcd2s::integration::PlayerAttach* order_of(std::uint32_t player) {
  const auto found = others.find(player);
  return found == others.end() ? nullptr : &found->second;
}

namespace {

// A matrix's place and its heading about the up axis (its rows, flat on the ground).
BodyPose pose_of(const float* matrix) {
  BodyPose pose;
  pose.x = matrix[3];
  pose.y = matrix[7];
  pose.z = matrix[11];
  pose.heading = static_cast<float>(std::atan2(double(matrix[4]), double(matrix[0])));
  return pose;
}

}  // namespace

kcd2s::integration::PlayerAttach order_now(std::uint32_t player) {
  const auto found = others.find(player);
  if (found == others.end()) {
    return {};
  }
  const auto since = others_since.find(player);
  const double elapsed = since == others_since.end() ? 1e9 : elapsed_ms(since->second);
  return kcd2s::integration::attach_offset_at(found->second, elapsed);
}

bool body_pose(std::uint32_t network_id, BodyPose& pose) {
  void* body = natives.body_entity ? natives.body_entity(network_id) : nullptr;
  if (!body) {
    return false;
  }
  pose = horse_pose(body);
  return true;
}

bool local_pose(BodyPose& pose) {
  void* body = natives.local_entity ? natives.local_entity() : nullptr;
  if (!body) {
    return false;
  }
  pose = pose_of(slot<GetWorldTM>(body, kGetWorldTM)(body));
  return true;
}

BodyPose beside(const kcd2s::integration::PlayerAttach& given, const BodyPose& target) {
  const double c = std::cos(double(target.heading));
  const double s = std::sin(double(target.heading));
  BodyPose pose;
  pose.x = target.x + static_cast<float>(given.right * c - given.forward * s);
  pose.y = target.y + static_cast<float>(given.right * s + given.forward * c);
  pose.z = target.z + given.up;
  pose.heading = target.heading + static_cast<float>(double(given.yaw) * kPi / 180.0);
  return pose;
}

void frame(bool in_world) {
  if (!in_world || !natives.local_entity || !natives.replica_entity) {
    held_body = nullptr;
    return;
  }
  keep_replicas();
  if (order.target == 0) {
    leave_seat(local_seat);
    release_body();
    return;
  }
  void* body = natives.local_entity();
  const bool to_body = (order.flags & kcd2s::integration::kAttachToBody) != 0;
  void* target = nullptr;
  if (to_body) {
    target = natives.body_entity ? natives.body_entity(order.target) : nullptr;
  } else {
    target = natives.replica_entity(order.target);
  }
  if (!body || !target) {
    return;
  }
  if (held_body != body) {
    set_physics(body, false);
    held_body = body;
    if (log_line) {
      log_line("ATTACH body attached to %s %u", to_body ? "body" : "player",
               unsigned(order.target));
    }
  }
  const BodyPose seen =
      to_body ? horse_pose(target) : pose_of(slot<GetWorldTM>(target, kGetWorldTM)(target));
  const double elapsed = elapsed_ms(order_since);
  const BodyPose place = beside(kcd2s::integration::attach_offset_at(order, elapsed), seen);
  // On a horse behind its rider: sitting on a seat on the rider's joint, holding on to the rider.
  if (seated(order, elapsed)) {
    kcds::runtime::riding::passenger_pose::hold(body, rider_seen(order.target, body));
    if (local_seat.anchor && (local_seat.horse != target || local_seat.body != body)) {
      if (local_seat.body != body) {
        local_seat.body = nullptr;
      }
      leave_seat(local_seat);
    }
    if (!local_seat.anchor && take_seat(local_seat, "kcds_seat_local", target, body, place)) {
      if (log_line) {
        log_line("ATTACH body sits on horse %u", unsigned(order.target));
      }
    }
    if (local_seat.anchor) {
      return;
    }
  } else {
    leave_seat(local_seat);
  }
  // The body turns with the target whatever the look: a rider stays on the shoulders the right way.
  const float c = std::cos(place.heading);
  const float s = std::sin(place.heading);
  const float matrix[12] = {c, -s, 0, place.x, s, c, 0, place.y, 0, 0, 1, place.z};
  slot<SetWorldTM>(body, kSetWorldTM)(body, matrix, 0);
}

bool local_attached() {
  return order.target != 0;
}

void* seated_body() {
  if (order.target == 0 || !local_seat.anchor || !local_seat.body) {
    return nullptr;
  }
  return parent_of(local_seat.body) == local_seat.anchor ? local_seat.body : nullptr;
}

void* attach_child_native() {
  return natives.attach_child;
}

bool free_look_wanted() {
  return order.target != 0 && (order.flags & kcd2s::integration::kAttachFollowLook) == 0;
}

bool refuse_action(const char* action, unsigned mode) {
  if (order.target == 0 || !action || mode == kReleased) {
    return false;
  }
  // The photo mode's camera flies with the movement keys and the mouse: the body stays put anyway.
  if (kcds::runtime::camera::photo_mode_open()) {
    return false;
  }
  for (const char* prefix : kKeptPrefixes) {
    if (std::strncmp(action, prefix, std::strlen(prefix)) == 0) {
      // look 'follow': no looking around either.
      const bool turning = std::strncmp(action, "rotate", 6) == 0;
      return turning && (order.flags & kcd2s::integration::kAttachFollowLook) != 0;
    }
  }
  return true;
}

}  // namespace kcds::runtime::attach
