#include "anchor_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kcds::runtime::camera {
namespace {

// gFramework (byte offsets): GetIViewSystem.
constexpr std::size_t kFrameworkViews = 232;
// CViewSystem (byte offsets) and its active view id.
constexpr std::size_t kCreateView = 8;
constexpr std::size_t kRemoveView = 16;
constexpr std::size_t kSetActiveView = 40;
constexpr std::size_t kGetView = 48;
constexpr std::size_t kGetViewId = 72;
constexpr std::size_t kActiveViewId = 96;
// CView (byte offsets): SetCurrentParams, GetCurrentParams; SViewParams as floats.
constexpr std::size_t kSetParams = 48;
constexpr std::size_t kGetParams = 56;
constexpr std::size_t kParamsSize = 212;
constexpr int kPosition = 0;
constexpr int kRotation = 3;
constexpr int kFov = 12;
constexpr int kLastPosition = 45;
constexpr int kLastRotation = 48;
constexpr int kLastFov = 52;
// CView: the id of the entity it is linked to.
constexpr std::size_t kViewLinkedId = 16;
// The entity system (byte offsets): the class registry and its FindClass, SpawnEntity(params,
// init), GetEntity(id), RemoveEntity(id, now); SEntitySpawnParams (144 bytes) and its fields.
constexpr std::size_t kClassRegistry = 88;
constexpr std::size_t kFindClass = 24;
constexpr std::size_t kSpawnEntity = 96;
constexpr std::size_t kGetEntity = 112;
constexpr std::size_t kRemoveEntity = 152;
constexpr std::size_t kSpawnParamsSize = 144;
constexpr std::size_t kSpawnClass = 24;
constexpr std::size_t kSpawnName = 56;
constexpr std::size_t kSpawnFlags = 64;
constexpr std::size_t kSpawnFlagsExtended = 68;
constexpr std::size_t kSpawnPosition = 80;
// The flags the script SpawnEntity adds.
constexpr std::uint32_t kSpawnScriptFlags = 0x1000000;
constexpr std::uint32_t kSpawnScriptFlagsExtended = 0x400000;
constexpr char kAnchorClass[] = "TagPoint";
// CEntity: its id, SetWorldTM(matrix, reason).
constexpr std::size_t kEntityId = 12;
constexpr std::size_t kSetWorldTM = 240;

constexpr double kPi = 3.14159265358979323846;

using Get = void*(__fastcall*)(void* object);
using GetView = void*(__fastcall*)(void* views, unsigned id);
using SetView = void(__fastcall*)(void* views, void* view);
using ViewId = unsigned(__fastcall*)(void* views, void* view);
using RemoveView = void(__fastcall*)(void* views, unsigned id);
using SetParams = void(__fastcall*)(void* view, const void* params);
using GetParams = const void*(__fastcall*)(void* view);
using FindClass = void*(__fastcall*)(void* registry, const char* name);
using SpawnParamsInit = void*(__fastcall*)(void* params);
using SpawnEntity = void*(__fastcall*)(void* entities, void* params, bool init);
using GetEntity = void*(__fastcall*)(void* entities, std::uint32_t id);
using RemoveEntity = void(__fastcall*)(void* entities, std::uint32_t id, bool now);
using SetWorldTM = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

double radians(double degrees) {
  return degrees * kPi / 180;
}

double degrees(double radians) {
  return radians * 180 / kPi;
}

// The pose of a view's parameters: its position, and the heading and elevation of its forward.
Pose pose_of(const float* params) {
  const double x = params[kRotation];
  const double y = params[kRotation + 1];
  const double z = params[kRotation + 2];
  const double w = params[kRotation + 3];
  // The quaternion turning +Y.
  const double fx = 2 * (x * y - w * z);
  const double fy = 1 - 2 * (x * x + z * z);
  const double fz = 2 * (y * z + w * x);
  Pose result;
  result.x = params[kPosition];
  result.y = params[kPosition + 1];
  result.z = params[kPosition + 2];
  result.yaw = degrees(std::atan2(-fx, fy));
  result.pitch = std::clamp(degrees(std::asin(std::clamp(fz, -1.0, 1.0))), -89.0, 89.0);
  return result;
}

// The pose's rotation quaternion (x, y, z, w): heading about +Z, then elevation about the view's
// right, then roll about its forward.
void rotation_of(const Pose& at, double q[4]) {
  const double half_yaw = radians(at.yaw) / 2;
  const double half_pitch = radians(at.pitch) / 2;
  const double half_roll = radians(at.roll) / 2;
  const double cz = std::cos(half_yaw);
  const double sz = std::sin(half_yaw);
  const double cx = std::cos(half_pitch);
  const double sx = std::sin(half_pitch);
  const double cr = std::cos(half_roll);
  const double sr = std::sin(half_roll);
  // Heading and elevation, then the roll about +Y multiplied on the right.
  const double ax = cz * sx;
  const double ay = sz * sx;
  const double az = sz * cx;
  const double aw = cz * cx;
  q[0] = ax * cr - az * sr;
  q[1] = aw * sr + ay * cr;
  q[2] = ax * sr + az * cr;
  q[3] = aw * cr - ay * sr;
}

// Writes the pose and field of view into a view's parameters.
void write_pose(float* params, const Pose& at, double fov_degrees) {
  double q[4];
  rotation_of(at, q);
  const float rotation[4] = {static_cast<float>(q[0]), static_cast<float>(q[1]),
                             static_cast<float>(q[2]), static_cast<float>(q[3])};
  const float position[3] = {static_cast<float>(at.x), static_cast<float>(at.y),
                             static_cast<float>(at.z)};
  for (int base : {kPosition, kLastPosition}) {
    std::memcpy(params + base, position, sizeof(position));
  }
  for (int base : {kRotation, kLastRotation}) {
    std::memcpy(params + base, rotation, sizeof(rotation));
  }
  params[kFov] = params[kLastFov] = static_cast<float>(radians(fov_degrees));
}

// The anchor's world matrix for the pose: its columns are the right, forward and up.
void place_anchor(void* entity, const Pose& at) {
  double q[4];
  rotation_of(at, q);
  const double x = q[0];
  const double y = q[1];
  const double z = q[2];
  const double w = q[3];
  // The rotation matrix of the quaternion, rows first, with the position as the fourth column.
  const float matrix[12] = {
      static_cast<float>(1 - 2 * (y * y + z * z)), static_cast<float>(2 * (x * y - w * z)),
      static_cast<float>(2 * (x * z + w * y)),     static_cast<float>(at.x),
      static_cast<float>(2 * (x * y + w * z)),     static_cast<float>(1 - 2 * (x * x + z * z)),
      static_cast<float>(2 * (y * z - w * x)),     static_cast<float>(at.y),
      static_cast<float>(2 * (x * z - w * y)),     static_cast<float>(2 * (y * z + w * x)),
      static_cast<float>(1 - 2 * (x * x + y * y)), static_cast<float>(at.z)};
  slot<SetWorldTM>(entity, kSetWorldTM)(entity, matrix, 0);
}

}  // namespace

Pose pose_of_rotation(double x, double y, double z, double w) {
  const double norm = std::sqrt(x * x + y * y + z * z + w * w);
  if (norm > 0) {
    x /= norm;
    y /= norm;
    z /= norm;
    w /= norm;
  }
  // The forward (+Y turned) gives heading and elevation.
  const double fx = 2 * (x * y - w * z);
  const double fy = 1 - 2 * (x * x + z * z);
  const double fz = 2 * (y * z + w * x);
  Pose pose;
  pose.yaw = degrees(std::atan2(-fx, fy));
  pose.pitch = std::clamp(degrees(std::atan2(fz, std::hypot(fx, fy))), -89.0, 89.0);
  // The right (+X turned) against the right and up of the same heading and elevation unrolled.
  const double rx = 1 - 2 * (y * y + z * z);
  const double ry = 2 * (x * y + w * z);
  const double rz = 2 * (x * z - w * y);
  const double yaw = radians(pose.yaw);
  const double pitch = radians(pose.pitch);
  const double along_right = rx * std::cos(yaw) + ry * std::sin(yaw);
  const double along_up = rx * std::sin(yaw) * std::sin(pitch) -
                          ry * std::cos(yaw) * std::sin(pitch) + rz * std::cos(pitch);
  pose.roll = degrees(std::atan2(-along_up, along_right));
  return pose;
}

void* AnchorView::views() const {
  void* framework = natives_.framework ? natives_.framework() : nullptr;
  return framework ? slot<Get>(framework, kFrameworkViews)(framework) : nullptr;
}

void* AnchorView::anchor() const {
  void* entities = natives_.entity_system ? natives_.entity_system() : nullptr;
  if (!entities || !anchor_id_) {
    return nullptr;
  }
  return slot<GetEntity>(entities, kGetEntity)(entities, anchor_id_);
}

bool AnchorView::begin(const Natives& natives, Pose* previous_pose, double* previous_fov) {
  natives_ = natives;
  void* system = views();
  void* entities = natives_.entity_system ? natives_.entity_system() : nullptr;
  if (!system || !entities || !natives_.spawn_params_init) {
    return false;
  }
  previous_view_ = *reinterpret_cast<unsigned*>(static_cast<std::uint8_t*>(system) + kActiveViewId);
  void* current = slot<GetView>(system, kGetView)(system, previous_view_);
  void* registry = slot<Get>(entities, kClassRegistry)(entities);
  void* klass = registry ? slot<FindClass>(registry, kFindClass)(registry, kAnchorClass) : nullptr;
  if (!current || !klass) {
    return false;
  }
  alignas(16) float params[kParamsSize / sizeof(float)];
  std::memcpy(params, slot<GetParams>(current, kGetParams)(current), kParamsSize);
  const Pose start = pose_of(params);
  if (previous_pose) {
    *previous_pose = start;
  }
  if (previous_fov) {
    *previous_fov = degrees(params[kFov]);
  }
  alignas(16) std::uint8_t spawn[kSpawnParamsSize];
  reinterpret_cast<SpawnParamsInit>(natives_.spawn_params_init)(spawn);
  std::memcpy(spawn + kSpawnClass, &klass, sizeof(klass));
  std::memcpy(spawn + kSpawnName, &anchor_name_, sizeof(anchor_name_));
  std::uint32_t flags = 0;
  std::memcpy(&flags, spawn + kSpawnFlags, sizeof(flags));
  flags |= kSpawnScriptFlags;
  std::memcpy(spawn + kSpawnFlags, &flags, sizeof(flags));
  std::memcpy(&flags, spawn + kSpawnFlagsExtended, sizeof(flags));
  flags |= kSpawnScriptFlagsExtended;
  std::memcpy(spawn + kSpawnFlagsExtended, &flags, sizeof(flags));
  const float position[3] = {static_cast<float>(start.x), static_cast<float>(start.y),
                             static_cast<float>(start.z)};
  std::memcpy(spawn + kSpawnPosition, position, sizeof(position));
  void* entity = slot<SpawnEntity>(entities, kSpawnEntity)(entities, spawn, true);
  void* camera = entity ? slot<Get>(system, kCreateView)(system) : nullptr;
  if (!camera) {
    if (entity) {
      const auto id =
          *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + kEntityId);
      slot<RemoveEntity>(entities, kRemoveEntity)(entities, id, false);
    }
    return false;
  }
  anchor_id_ = *reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(entity) + kEntityId);
  camera_view_ = slot<ViewId>(system, kGetViewId)(system, camera);
  place_anchor(entity, start);
  std::memcpy(static_cast<std::uint8_t*>(camera) + kViewLinkedId, &anchor_id_, sizeof(anchor_id_));
  slot<SetParams>(camera, kSetParams)(camera, params);
  slot<SetView>(system, kSetActiveView)(system, camera);
  return true;
}

bool AnchorView::place(const Pose& pose, double fov_degrees) {
  void* system = views();
  void* camera =
      system && anchor_id_ ? slot<GetView>(system, kGetView)(system, camera_view_) : nullptr;
  void* entity = anchor();
  if (!camera || !entity) {
    return false;
  }
  place_anchor(entity, pose);
  alignas(16) float params[kParamsSize / sizeof(float)];
  std::memcpy(params, slot<GetParams>(camera, kGetParams)(camera), kParamsSize);
  write_pose(params, pose, fov_degrees);
  slot<SetParams>(camera, kSetParams)(camera, params);
  return true;
}

void AnchorView::end() {
  if (void* system = views()) {
    if (void* previous = slot<GetView>(system, kGetView)(system, previous_view_)) {
      slot<SetView>(system, kSetActiveView)(system, previous);
    }
    if (camera_view_ && slot<GetView>(system, kGetView)(system, camera_view_)) {
      slot<RemoveView>(system, kRemoveView)(system, camera_view_);
    }
  }
  void* entities = natives_.entity_system ? natives_.entity_system() : nullptr;
  if (entities && anchor()) {
    slot<RemoveEntity>(entities, kRemoveEntity)(entities, anchor_id_, false);
  }
  anchor_id_ = 0;
  camera_view_ = 0;
}

}  // namespace kcds::runtime::camera
