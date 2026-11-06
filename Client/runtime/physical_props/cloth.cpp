// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "cloth.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../props/prop_entity.hpp"

namespace kcds::runtime::physical_props::cloth {
namespace {

using game_client::slot;

constexpr std::size_t kGetGeomManager = 4;
constexpr std::size_t kCreateMesh = 3;
constexpr std::size_t kRegisterGeometry = 7;
constexpr std::size_t kEntityGetStatObj = 103;
constexpr std::size_t kSetPhysGeom = 27;
constexpr int kMeshSlot = 0;
constexpr int kDefaultPhysGeom = 0x1000;
// CreateMesh's flags (as reads them): 0x20 an OBB tree, 0x40 an AABB tree (it builds
// the better one), 0x8000 the vertex map kept. Without a tree the mesh has no bounding box and the
// soft body's AddGeometry faults on it.
constexpr int kMeshFlags = 0x20 | 0x40 | 0x8000;

// strided_pointer: the data and the bytes from one item to the next.
struct Strided {
  const void* data;
  std::int32_t stride;
  std::int32_t unused;
};

// SMeshBVParams as CreateMesh reads it (three words).
struct BoundingParams {
  std::int32_t min_triangles;
  std::int32_t max_triangles;
  float favour_box;
};

using GetGeomManager = void*(__fastcall*)(void* world);
using CreateMesh = void*(__fastcall*)(void* manager, const Strided* vertices,
                                      const Strided* indices, const char* materials,
                                      const int* foreign_ids, int triangles, int flags,
                                      float tolerance, const BoundingParams* params);
using RegisterGeometry = void*(__fastcall*)(void* manager, void* geometry, int surface,
                                            const int* materials, int count);
using GetStatObj = void*(__fastcall*)(void* entity, int slot);
using GetPhysics = void*(__fastcall*)(void* entity);
using GetType = int(__fastcall*)(void* body);
using PhysicsCall = int(__fastcall*)(void* body, const void* params, int thread_safe);
using SetPhysGeom = void(__fastcall*)(void* model, void* geometry, int type);
// IGeometry::SetForeignData (slot 38, + 304; the physics proxy's sync calls it).
constexpr std::size_t kGeometrySetForeignData = 38;
using SetForeignData = void(__fastcall*)(void* geometry, void* data, int id);

constexpr std::size_t kEntityGetPhysics = 79;
constexpr std::size_t kPhysicsGetType = 1;
constexpr std::size_t kPhysicsAction = 7;
constexpr std::uint32_t kActionAwake = 8;
constexpr std::uint32_t kUnusedFloat = 0xFFBFFFFF;
constexpr int kSoftType = 8;
constexpr std::uint32_t kActionAttachPoints = 11;
constexpr std::intptr_t kTiedToWorld = -10;
constexpr std::uint32_t kUnusedInt = 0x80000000;
// The body's simulation class (byte + 71; -1 while its physics is off, 7 and more while it goes
// away) and its attached-vertex count (+ 380, what pe_action_attach_points leaves there).
constexpr std::size_t kSimulationClass = 71;
constexpr std::uint8_t kSuspended = 0xFF;
constexpr std::uint8_t kFirstGone = 7;
constexpr std::size_t kAttachedCount = 380;
// The entity's physics proxy (CEntity slot 74, proxy 1) with its EnablePhysics (slot 24).
constexpr std::size_t kEntityGetProxy = 74;
constexpr int kPhysicsProxy = 1;
constexpr std::size_t kEnablePhysics = 24;
using GetProxy = void*(__fastcall*)(void* entity, int type);
using EnablePhysics = void(__fastcall*)(void* proxy, bool enable);

void* const* world_slot = nullptr;

// The cloth made last, until its pinned vertices hold.
struct Cloth {
  void* entity = nullptr;
  std::vector<int> pinned;
  bool attached = false;
};
std::optional<Cloth> cloth_made;

void* body_of(void* entity) {
  return entity ? slot<GetPhysics>(entity, kEntityGetPhysics)(entity) : nullptr;
}

// pe_action_attach_points: the vertices held where they are, tied to the world. The game's
// change-request gate runs it at once, queues it while the world steps, and drops
// it (answering 1 all the same) while the body's physics is off: so tick() sends it again until
// the body counts the vertices attached.
void attach(void* body, const std::vector<int>& pinned) {
  alignas(8) std::uint8_t action[48] = {};
  const std::int32_t type = kActionAttachPoints;
  const std::intptr_t world = kTiedToWorld;
  const std::int32_t any_part = std::int32_t(kUnusedInt);
  const int* numbers = pinned.data();
  const std::intptr_t where_they_are = -1;
  const std::int32_t count = std::int32_t(pinned.size());
  std::memcpy(action + 0, &type, 4);
  std::memcpy(action + 8, &world, 8);
  std::memcpy(action + 16, &any_part, 4);
  std::memcpy(action + 24, &numbers, 8);
  std::memcpy(action + 32, &where_they_are, 8);
  std::memcpy(action + 40, &count, 4);
  slot<PhysicsCall>(body, kPhysicsAction)(body, action, 0);
  const std::uint32_t awake[3] = {kActionAwake, 1, kUnusedFloat};
  slot<PhysicsCall>(body, kPhysicsAction)(body, awake, 0);
}

std::int32_t attached_count(const void* body) {
  std::int32_t count = 0;
  std::memcpy(&count, static_cast<const std::uint8_t*>(body) + kAttachedCount, sizeof(count));
  return count;
}

}  // namespace

void install(void* const* physical_world) {
  world_slot = physical_world;
}

void keep_on(void* entity) {
  void* body = body_of(entity);
  if (!body || static_cast<const std::uint8_t*>(body)[kSimulationClass] != kSuspended) {
    return;
  }
  void* proxy = slot<GetProxy>(entity, kEntityGetProxy)(entity, kPhysicsProxy);
  if (proxy) {
    slot<EnablePhysics>(proxy, kEnablePhysics)(proxy, true);
  }
}

void* give_collision(void* entity, const std::vector<float>& vertices,
                     const std::vector<std::uint16_t>& indices) {
  void* world = world_slot ? *world_slot : nullptr;
  if (!entity || !world || vertices.size() < 9 || indices.size() < 3 || indices.size() % 3) {
    game_client::log("PHYSICS MESH: no entity, physics or mesh");
    return nullptr;
  }
  void* manager = slot<GetGeomManager>(world, kGetGeomManager)(world);
  void* model = slot<GetStatObj>(entity, kEntityGetStatObj)(entity, kMeshSlot);
  if (!manager || !model) {
    game_client::log("PHYSICS MESH: no geometry manager (%p) or model (%p)", manager, model);
    return nullptr;
  }
  const Strided points{vertices.data(), sizeof(float) * 3, 0};
  const Strided corners{indices.data(), sizeof(std::uint16_t), 0};
  const BoundingParams params{2, 4, 1.0f};
  const int triangles = int(indices.size() / 3);
  void* geometry = slot<CreateMesh>(manager, kCreateMesh)(
      manager, &points, &corners, nullptr, nullptr, triangles, kMeshFlags, 0.0f, &params);
  void* physics = nullptr;
  if (geometry) {
    physics = slot<RegisterGeometry>(manager, kRegisterGeometry)(manager, geometry, 0, nullptr, 0);
  }
  if (!physics) {
    game_client::log("PHYSICS MESH: the game made no geometry of %d triangles", triangles);
    return nullptr;
  }
  slot<SetPhysGeom>(model, kSetPhysGeom)(model, physics, kDefaultPhysGeom);
  return geometry;
}

bool make(void* entity, const std::vector<float>& vertices,
          const std::vector<std::uint16_t>& indices, float mass, const std::vector<int>& pinned) {
  void* geometry = give_collision(entity, vertices, indices);
  if (!geometry) {
    game_client::log("CLOTH: no physics mesh");
    return false;
  }
  void* model = slot<GetStatObj>(entity, kEntityGetStatObj)(entity, kMeshSlot);
  const int triangles = int(indices.size() / 3);
  // The mesh's foreign data is its model, as a model loaded from a .cgf has it: the model's
  // SyncToRenderMesh draws the cloth's vertices only while it finds itself (or the
  // model it was cloned from, +544) there (null without this, the cloth stayed flat).
  slot<SetForeignData>(geometry, kGeometrySetForeignData)(geometry, model, 0);
  const bool soft = props::entity::physicalize(entity, props::entity::Physics::Soft, mass);
  void* body = body_of(entity);
  const int type = body ? slot<GetType>(body, kPhysicsGetType)(body) : -1;
  game_client::log("CLOTH: %d triangles, %.2f kg, %zu pinned: %s, body %p type %d", triangles, mass,
                   pinned.size(), soft ? "soft" : "not physicalized", body, type);
  if (!soft || type != kSoftType) {
    cloth_made.reset();
    return false;
  }
  cloth_made = Cloth{entity, pinned, false};
  tick();
  return true;
}

void tick() {
  if (!cloth_made) {
    return;
  }
  void* body = body_of(cloth_made->entity);
  if (!body) {
    return;
  }
  keep_on(cloth_made->entity);
  if (cloth_made->attached || cloth_made->pinned.empty()) {
    return;
  }
  if (attached_count(body) > 0) {
    cloth_made->attached = true;
    game_client::log("CLOTH: %d vertices held", attached_count(body));
    return;
  }
  const std::uint8_t simulation = static_cast<const std::uint8_t*>(body)[kSimulationClass];
  if (simulation < kFirstGone) {
    attach(body, cloth_made->pinned);
  }
}

void forget(void* entity) {
  if (cloth_made && (!entity || cloth_made->entity == entity)) {
    cloth_made.reset();
  }
}

}  // namespace kcds::runtime::physical_props::cloth
