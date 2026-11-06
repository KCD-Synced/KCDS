#include "local_world.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../level/level_edits.hpp"
#include "../props/pose_math.hpp"
#include "../props/prop_entity.hpp"
#include "kcd2s/integration/level_edits.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "line_of_sight.hpp"
#include "world_raycast.hpp"

namespace kcds::runtime::physics {

namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// IPhysicalEntity: GetForeignData(type) (slot 10); type 1 is a static render node
// (PHYS_FOREIGN_ID_STATIC).
constexpr std::size_t kForeignData = 10;
constexpr int kForeignStatic = 1;
// IPhysicalWorld: GetEntitiesInBox(min, max, list, types, preallocated) (slot 34): static,
// sleeping and moving rigid, living and independent entities.
constexpr std::size_t kEntitiesInBox = 34;
constexpr int kEveryPhysicalKind = 0x1F;
// IRenderNode GetName (slot 9): a brush's or a vegetation instance's mesh path.
constexpr std::size_t kRenderNodeName = 9;
// IRenderNode GetBBox (slot 5, the AABB written out: CBrush); a box wider than this is
// not taken (a level's merged node, a terrain sector).
constexpr std::size_t kRenderNodeBox = 5;
constexpr float kLargestBox = 400.0f;
constexpr char kBrushClass[] = ".?AVCBrush@@";
constexpr char kVegetationClass[] = ".?AVCVegetation@@";
// IEntity: GetClass (slot 3), its key, the EntityGuid (slot 2), GetPhysics (slot 79);
// IEntityClass GetName (slot 2). IEntitySystem's iterator (slot 22): AddRef 1, Release 2, Next 4,
// MoveFirst 6.
constexpr std::size_t kEntityClass = 3;
constexpr std::size_t kEntityKey = 2;
constexpr std::size_t kGetPhysics = 79;
constexpr std::size_t kClassName = 2;
constexpr std::size_t kIteratorSlot = 22;
constexpr std::size_t kIteratorAddRef = 1;
constexpr std::size_t kIteratorRelease = 2;
constexpr std::size_t kIteratorNext = 4;
constexpr std::size_t kIteratorMoveFirst = 6;
// The limits the scripts' side checks too (Client/resources/world.cjs).
constexpr int kMaxHits = 8;
constexpr float kMaxRayLength = 4096.0f;
constexpr float kMaxRadius = 256.0f;
constexpr int kMaxEntities = 64;
constexpr std::size_t kMaxText = 256;
// How far past a hit the next trace of every hit starts (metres).
constexpr float kStepPast = 0.02f;
// The most requests kept between two frames.
constexpr std::size_t kMaxQueued = 256;

struct Request {
  std::int64_t id = 0;
  sdk::Value body;
};

struct Reply {
  std::int64_t id = 0;
  sdk::Value value;
};

LocalWorldNatives natives;
std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<Reply> replies;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

// The object's RTTI class name (".?AV...@@"), empty when it has none. The x64 complete object
// locator before the vtable holds its type descriptor's offset (+ 12) and its own (+ 20) from the
// module's base; the name is at the descriptor's + 16.
const char* rtti_name(void* object) {
  void** vtable = *static_cast<void***>(object);
  const auto* locator = static_cast<const std::uint8_t*>(vtable[-1]);
  if (!locator) {
    return "";
  }
  std::uint32_t descriptor_offset = 0;
  std::uint32_t own_offset = 0;
  std::memcpy(&descriptor_offset, locator + 12, sizeof(descriptor_offset));
  std::memcpy(&own_offset, locator + 20, sizeof(own_offset));
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(locator) - own_offset;
  return reinterpret_cast<const char*>(base + descriptor_offset + 16);
}

double number(const sdk::Value& request, const char* name, double fallback) {
  const sdk::Value* value = integration::script_field(request, name);
  if (!value) {
    return fallback;
  }
  if (value->tag() == sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == sdk::Tag::Int) {
    return double(value->as_int());
  }
  return fallback;
}

bool flag(const sdk::Value& request, const char* name, bool fallback) {
  const sdk::Value* value = integration::script_field(request, name);
  return value && value->tag() == sdk::Tag::Bool ? value->as_bool() : fallback;
}

std::string text(const sdk::Value& request, const char* name) {
  const sdk::Value* value = integration::script_field(request, name);
  return value && value->tag() == sdk::Tag::Str ? value->as_str() : std::string();
}

// A point { x, y, z } of finite numbers; false for anything else.
bool point(const sdk::Value& request, const char* name, float out[3]) {
  const sdk::Value* value = integration::script_field(request, name);
  if (!value || value->tag() != sdk::Tag::Map) {
    return false;
  }
  const char* axes[3] = {"x", "y", "z"};
  for (int axis = 0; axis < 3; ++axis) {
    const double component = number(*value, axes[axis], NAN);
    if (!std::isfinite(component)) {
      return false;
    }
    out[axis] = float(component);
  }
  return true;
}

sdk::Value vector_value(const float v[3]) {
  return sdk::Value(sdk::Value::Array{sdk::Value(double(v[0])), sdk::Value(double(v[1])),
                                      sdk::Value(double(v[2]))});
}

std::string cut(const char* value) {
  return value ? std::string(value, strnlen(value, kMaxText)) : std::string();
}

std::string class_of(void* entity) {
  using Get = void*(__fastcall*)(void*);
  using Name = const char*(__fastcall*)(void*);
  void* klass = slot<Get>(entity, kEntityClass)(entity);
  return klass ? cut(slot<Name>(klass, kClassName)(klass)) : std::string();
}

// Its EntityGuid as sixteen hex digits in the level files' order; empty for an entity the level
// does not name (one this session spawned).
std::string guid_of(void* entity) {
  using Key = std::uint64_t(__fastcall*)(void*);
  const std::uint64_t key = slot<Key>(entity, kEntityKey)(entity);
  return key ? integration::level_edit_guid_text(key) : std::string();
}

bool physicalized(void* entity) {
  using GetPhysics = void*(__fastcall*)(void*);
  return slot<GetPhysics>(entity, kGetPhysics)(entity) != nullptr;
}

void* own_physics() {
  void* entity = natives.local_entity ? natives.local_entity() : nullptr;
  if (!entity) {
    return nullptr;
  }
  using GetPhysics = void*(__fastcall*)(void*);
  return slot<GetPhysics>(entity, kGetPhysics)(entity);
}

int object_types(int mode) {
  if (mode == 1) {
    return kHitAnything;
  }
  if (mode == 2) {
    return kHitGround;
  }
  if (mode == 3) {
    return kHitTerrainOnly;
  }
  return kHitCover;
}

// Whether a mesh is one of the level's HLODs (hlods/main/Main_6/Main_6_8/Main_6_8_12_<hash>.cgf):
// a cell's far, merged stand-in, whose collision also carries the cell's small props (a table, a
// bench), which have none of their own (a table picked as main_6_8_12).
bool is_hlod(const std::string& mesh) {
  std::string lower = mesh;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](char c) { return char(std::tolower(unsigned(c))); });
  return lower.rfind("hlods/", 0) == 0 || lower.find("/hlods/") != std::string::npos ||
         lower.rfind("main_", 0) == 0;
}

// The level brush a ray met whose static physics carries no foreign data (most of a village's
// houses and props): the shown brush of the 3D engine's object tree
// whose bounds reach the hit, the one whose origin is nearest it. Empty when there is none.
bool brush_at(const float position[3], level::LevelBrush& found) {
  constexpr float kReach = 0.05f;
  bool any = false;
  float nearest = 0;
  for (const level::LevelBrush& brush : level::brushes_near(position, kReach)) {
    if (brush.hidden || brush.mesh.empty() || is_hlod(brush.mesh)) {
      continue;
    }
    const float dx = brush.matrix[3] - position[0];
    const float dy = brush.matrix[7] - position[1];
    const float dz = brush.matrix[11] - position[2];
    const float reach = dx * dx + dy * dy + dz * dz;
    if (!any || reach < nearest) {
      found = brush;
      nearest = reach;
      any = true;
    }
  }
  return any;
}

// A brush's level object from its world matrix (3x4 by rows): its translation, turn and the length
// of each axis.
void brush_object(HitDescription& hit, const float matrix[12]) {
  hit.object = 1;
  hit.object_at[0] = matrix[3];
  hit.object_at[1] = matrix[7];
  hit.object_at[2] = matrix[11];
  pose_math::from_matrix(matrix, hit.object_rotation);
  pose_math::normalize(hit.object_rotation);
  for (int axis = 0; axis < 3; ++axis) {
    hit.object_scale[axis] =
        std::sqrt(matrix[axis] * matrix[axis] + matrix[4 + axis] * matrix[4 + axis] +
                  matrix[8 + axis] * matrix[8 + axis]);
  }
}

// What the ray met, as the scripts read it.
sdk::Value hit_value(const RayHit& hit, float distance) {
  const HitDescription described = describe_hit(hit);
  sdk::Value object;
  if (described.object != 0) {
    const bool brush = described.object == 1;
    object = sdk::Value(sdk::Value::Map{
        {"kind", sdk::Value(std::string(brush ? "brush" : "entity"))},
        {"name", sdk::Value(brush ? described.model : described.name)},
        {"guid", sdk::Value(described.guid)},
        {"cls", sdk::Value(described.entity_class)},
        {"at", vector_value(described.object_at)},
        {"rot", sdk::Value(sdk::Value::Array{sdk::Value(double(described.object_rotation[0])),
                                             sdk::Value(double(described.object_rotation[1])),
                                             sdk::Value(double(described.object_rotation[2])),
                                             sdk::Value(double(described.object_rotation[3]))})},
        {"scale", vector_value(described.object_scale)},
        {"unseen", sdk::Value(described.unseen)},
    });
  }
  return sdk::Value(sdk::Value::Map{
      {"p", vector_value(hit.position)},
      {"n", vector_value(hit.normal)},
      {"d", sdk::Value(double(distance))},
      {"terrain", sdk::Value(hit.terrain)},
      {"surface", sdk::Value(surface_type_name(hit.surface))},
      {"kind", sdk::Value(described.kind)},
      {"model", sdk::Value(described.model)},
      {"entity", sdk::Value(std::int64_t(described.entity_id))},
      {"cls", sdk::Value(described.entity_class)},
      {"name", sdk::Value(described.name)},
      {"guid", sdk::Value(described.guid)},
      {"lo", object},
      {"box", described.has_box
                  ? sdk::Value(sdk::Value::Array{
                        sdk::Value(double(described.box[0])), sdk::Value(double(described.box[1])),
                        sdk::Value(double(described.box[2])), sdk::Value(double(described.box[3])),
                        sdk::Value(double(described.box[4])), sdk::Value(double(described.box[5]))})
                  : sdk::Value()},
  });
}

sdk::Value ray_reply(const char* status, sdk::Value::Array hits) {
  return sdk::Value(sdk::Value::Map{{"status", sdk::Value(std::string(status))},
                                    {"hits", sdk::Value(std::move(hits))}});
}

// Every hit along the ray, nearest first, up to `max`.
sdk::Value answer_ray(const sdk::Value& request, bool in_world) {
  float from[3];
  float to[3];
  if (!in_world || !point(request, "from", from) || !point(request, "to", to)) {
    return ray_reply("unavailable", {});
  }
  const float direction[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
  const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                 direction[2] * direction[2]);
  if (!(length > 0.0f) || length > kMaxRayLength) {
    return ray_reply("unavailable", {});
  }
  const int mode = int(number(request, "mode", 0));
  const int max = std::clamp(int(number(request, "max", 1)), 1, kMaxHits);
  std::vector<void*> skipped;
  if (flag(request, "ignoreSelf", true)) {
    if (void* own = own_physics()) {
      skipped.push_back(own);
    }
  }
  sdk::Value::Array hits;
  float travelled = 0.0f;
  while (int(hits.size()) < max && travelled < length) {
    float start[3];
    for (int axis = 0; axis < 3; ++axis) {
      start[axis] = from[axis] + direction[axis] * (travelled / length);
    }
    RayHit hit;
    const TraceResult result =
        trace(start, to, object_types(mode), skipped.data(), int(skipped.size()), hit);
    if (result == TraceResult::Unavailable) {
      if (hits.empty()) {
        return ray_reply("unavailable", {});
      }
      break;
    }
    if (result == TraceResult::Miss) {
      break;
    }
    hits.push_back(hit_value(hit, travelled + hit.distance));
    travelled += hit.distance + kStepPast;
    // Terrain is stepped past, never skipped; nothing lies behind it for a ray to find anyway.
    if (hit.terrain) {
      break;
    }
    if (hit.collider) {
      skipped.push_back(hit.collider);
    }
  }
  return ray_reply(hits.empty() ? "miss" : "hit", std::move(hits));
}

struct Nearby {
  void* entity = nullptr;
  float position[3] = {0.0f, 0.0f, 0.0f};
  float distance = 0.0f;
};

// The entity at `entity`, if it stands within the sphere and is of the class asked for.
void consider(void* entity, const float center[3], float radius, const std::string& wanted,
              std::vector<Nearby>& found) {
  if (!entity) {
    return;
  }
  Nearby close;
  close.entity = entity;
  props::entity::world_position(entity, close.position);
  const float dx = close.position[0] - center[0];
  const float dy = close.position[1] - center[1];
  const float dz = close.position[2] - center[2];
  close.distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!(close.distance <= radius)) {
    return;
  }
  if (!wanted.empty() && class_of(entity) != wanted) {
    return;
  }
  found.push_back(close);
}

// The entities with physics whose colliders reach into the sphere's box.
void physical_entities(const float center[3], float radius, const std::string& wanted,
                       std::vector<Nearby>& found) {
  void* world = natives.physical_world ? *natives.physical_world : nullptr;
  if (!world) {
    return;
  }
  const float min[3] = {center[0] - radius, center[1] - radius, center[2] - radius};
  const float max[3] = {center[0] + radius, center[1] + radius, center[2] + radius};
  using EntitiesInBox = int(__fastcall*)(void*, const float*, const float*, void***, int, int);
  void** list = nullptr;
  const int count =
      slot<EntitiesInBox>(world, kEntitiesInBox)(world, min, max, &list, kEveryPhysicalKind, 0);
  std::unordered_set<void*> seen;
  for (int index = 0; list && index < count; ++index) {
    // An entity with several colliders comes once.
    void* entity = entity_of(list[index]);
    if (entity && seen.insert(entity).second) {
      consider(entity, center, radius, wanted, found);
    }
  }
}

// Every entity this game has, each one's place compared.
void every_entity(const float center[3], float radius, const std::string& wanted,
                  std::vector<Nearby>& found) {
  void* system = natives.entity_system ? natives.entity_system() : nullptr;
  if (!system) {
    return;
  }
  using Get = void*(__fastcall*)(void*);
  using Call = void(__fastcall*)(void*);
  void* iterator = slot<Get>(system, kIteratorSlot)(system);
  if (!iterator) {
    return;
  }
  slot<Call>(iterator, kIteratorAddRef)(iterator);
  slot<Call>(iterator, kIteratorMoveFirst)(iterator);
  while (void* entity = slot<Get>(iterator, kIteratorNext)(iterator)) {
    consider(entity, center, radius, wanted, found);
  }
  slot<Call>(iterator, kIteratorRelease)(iterator);
}

sdk::Value answer_entities(const sdk::Value& request, bool in_world) {
  float center[3];
  const float radius = float(number(request, "radius", 0));
  if (!in_world || !point(request, "center", center) || !(radius > 0.0f) || radius > kMaxRadius) {
    return sdk::Value(sdk::Value::Array{});
  }
  const std::string wanted = text(request, "cls");
  const int max = std::clamp(int(number(request, "max", kMaxEntities)), 1, kMaxEntities);
  const bool physical_only = flag(request, "physicalOnly", false);
  std::vector<Nearby> found;
  if (physical_only) {
    physical_entities(center, radius, wanted, found);
  } else {
    every_entity(center, radius, wanted, found);
  }
  std::sort(found.begin(), found.end(),
            [](const Nearby& a, const Nearby& b) { return a.distance < b.distance; });
  sdk::Value::Array entities;
  for (const Nearby& close : found) {
    if (int(entities.size()) >= max) {
      break;
    }
    const bool has_physics = physicalized(close.entity);
    entities.push_back(sdk::Value(sdk::Value::Map{
        {"entity", sdk::Value(std::int64_t(props::entity::id_of(close.entity)))},
        {"p", vector_value(close.position)},
        {"d", sdk::Value(double(close.distance))},
        {"cls", sdk::Value(class_of(close.entity))},
        {"name", sdk::Value(props::entity::name_of(close.entity, kMaxText))},
        {"guid", sdk::Value(guid_of(close.entity))},
        {"physical", sdk::Value(has_physics)},
    }));
  }
  return sdk::Value(std::move(entities));
}

}  // namespace

bool view_camera(float matrix[12], float& fov) {
  return natives.view_camera && natives.view_camera(matrix, fov);
}

sdk::Value first_hit(const float from[3], const float to[3], int mode) {
  std::vector<void*> skipped;
  if (void* own = own_physics()) {
    skipped.push_back(own);
  }
  RayHit hit;
  const TraceResult result =
      trace(from, to, object_types(mode), skipped.data(), int(skipped.size()), hit);
  return result == TraceResult::Hit ? hit_value(hit, hit.distance) : sdk::Value();
}

namespace {

sdk::Value answer_camera(bool in_world) {
  float matrix[12];
  float fov = 0.0f;
  if (!in_world || !natives.view_camera || !natives.view_camera(matrix, fov)) {
    return sdk::Value();
  }
  sdk::Value::Array rows;
  for (float value : matrix) {
    rows.push_back(sdk::Value(double(value)));
  }
  return sdk::Value(
      sdk::Value::Map{{"m", sdk::Value(std::move(rows))}, {"fov", sdk::Value(double(fov))}});
}

// Where this machine's game draws an entity now, or a joint of its body: a replica
// between its network updates, as the player sees it. Null when the entity or the joint is not
// here.
sdk::Value answer_pose(const sdk::Value& request, bool in_world) {
  const double id = number(request, "entity", 0);
  void* entity = in_world && id > 0 ? props::entity::by_id(std::uint32_t(id)) : nullptr;
  if (!entity) {
    return sdk::Value();
  }
  float position[3] = {};
  const std::string wanted = text(request, "joint");
  if (wanted.empty()) {
    props::entity::world_position(entity, position);
    return sdk::Value(sdk::Value::Map{{"p", vector_value(position)}});
  }
  const std::string joint = props::entity::body_joint_name(entity, wanted);
  float rotation[4] = {};
  if (joint.empty() || !props::entity::joint_pose(entity, joint.c_str(), position, rotation)) {
    return sdk::Value();
  }
  // The joint's turn in the world too (x, y, z, w): Player.jointRotation.
  sdk::Value::Array turn;
  for (const float part : rotation) {
    turn.push_back(sdk::Value(double(part)));
  }
  return sdk::Value(
      sdk::Value::Map{{"p", vector_value(position)}, {"q", sdk::Value(std::move(turn))}});
}

sdk::Value answer(const sdk::Value& request, bool in_world) {
  const std::string op = integration::script_operation(request);
  if (op == "localWorldRay") {
    return answer_ray(request, in_world);
  }
  if (op == "localWorldEntities") {
    return answer_entities(request, in_world);
  }
  if (op == "localWorldCamera") {
    return answer_camera(in_world);
  }
  if (op == "localWorldPose") {
    return answer_pose(request, in_world);
  }
  return sdk::Value();
}

}  // namespace

sdk::Value script_hit(const RayHit& hit, float distance) {
  return hit_value(hit, distance);
}

// A render node's box in the world (IRenderNode GetBBox, slot 5, an AABB it writes out).
void node_box(void* node, HitDescription& described) {
  if (!node) {
    return;
  }
  using GetBox = void(__fastcall*)(void* node, float* box);
  alignas(16) float box[6] = {};
  slot<GetBox>(node, kRenderNodeBox)(node, box);
  const bool sane = box[3] >= box[0] && box[4] >= box[1] && box[5] >= box[2] &&
                    box[3] - box[0] < kLargestBox && box[4] - box[1] < kLargestBox;
  if (sane) {
    std::copy(box, box + 6, described.box);
    described.has_box = true;
  }
}

// An entity as a hit on it describes it.
static void describe_entity(void* entity, HitDescription& described) {
  described.kind = "entity";
  described.entity_id = props::entity::id_of(entity);
  described.entity_class = class_of(entity);
  described.name = props::entity::name_of(entity, kMaxText);
  described.guid = guid_of(entity);
  described.has_box = props::entity::world_bounds(entity, described.box, described.box + 3);
  // An entity the level names: found again by its GUID, standing where it stands now.
  if (!described.guid.empty()) {
    described.object = 2;
    props::entity::world_pose(entity, described.object_at, described.object_rotation);
    const float size = props::entity::world_scale(entity);
    for (float& axis : described.object_scale) {
      axis = size;
    }
  }
}

HitDescription describe_hit(const RayHit& hit) {
  HitDescription described;
  if (hit.terrain) {
    described.kind = "terrain";
  } else if (void* entity = entity_of(hit.collider)) {
    describe_entity(entity, described);
  } else if (hit.collider) {
    using ForeignData = void*(__fastcall*)(void*, int);
    using NodeName = const char*(__fastcall*)(void*);
    void* node = slot<ForeignData>(hit.collider, kForeignData)(hit.collider, kForeignStatic);
    if (node) {
      const char* type = rtti_name(node);
      if (std::strcmp(type, kBrushClass) == 0) {
        described.kind = "brush";
      } else if (std::strcmp(type, kVegetationClass) == 0) {
        described.kind = "vegetation";
      }
      if (described.kind != "static") {
        described.model = cut(slot<NodeName>(node, kRenderNodeName)(node));
      }
      // A brush or a vegetation instance is a level object of kind 'brush': a level
      // edit finds it by its mesh and place.
      float matrix[12];
      if (described.kind != "static" && level::render_node_matrix(node, matrix)) {
        brush_object(described, matrix);
      }
      if (described.kind != "static") {
        node_box(node, described);
        described.unseen = level::brush_unseen(node);
      }
      // An HLOD's collision: the level's own brush there (the table, not the cell's stand-in).
      level::LevelBrush brush;
      if (described.kind == "brush" && is_hlod(described.model) && brush_at(hit.position, brush)) {
        described.model = cut(brush.mesh.c_str());
        brush_object(described, brush.matrix);
        node_box(brush.brush, described);
      }
    } else {
      // A building's own collider names the brush at its place; else the brush nearest the hit.
      level::LevelBrush brush;
      if (level::brush_of_collider(hit.collider, brush) || brush_at(hit.position, brush)) {
        described.kind = "brush";
        described.model = cut(brush.mesh.c_str());
        brush_object(described, brush.matrix);
        node_box(brush.brush, described);
      }
    }
  }
  return described;
}

// Whether an entity is this session's own and not the level's: the local player's body, and the
// replicas and props KCDS names kcd2s_... or kcds_....
static bool session_entity(void* entity, void* own) {
  if (entity == own) {
    return true;
  }
  const std::string name = props::entity::name_of(entity, kMaxText);
  return name.rfind("kcd2s_", 0) == 0 || name.rfind("kcds_", 0) == 0;
}

std::vector<NearEntity> level_entities_near(const float center[3], float radius, std::size_t most) {
  std::vector<std::pair<float, void*>> near;
  void* own = natives.local_entity ? natives.local_entity() : nullptr;
  props::entity::each_entity([&](void* entity) {
    float at[3];
    props::entity::world_position(entity, at);
    const float dx = at[0] - center[0];
    const float dy = at[1] - center[1];
    const float dz = at[2] - center[2];
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Only what the level names: the session's own entities have no GUID.
    if (distance <= radius && !guid_of(entity).empty() && !session_entity(entity, own)) {
      near.emplace_back(distance, entity);
    }
  });
  std::sort(near.begin(), near.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<NearEntity> result;
  for (const auto& [distance, entity] : near) {
    if (result.size() >= most) {
      break;
    }
    NearEntity found;
    describe_entity(entity, found.described);
    props::entity::world_position(entity, found.position);
    found.distance = distance;
    result.push_back(std::move(found));
  }
  return result;
}

void install_local_world(const LocalWorldNatives& given) {
  natives = given;
}

std::vector<void*> physical_entities_near(const float center[3], float radius) {
  std::vector<Nearby> found;
  physical_entities(center, radius, std::string(), found);
  std::vector<void*> entities;
  entities.reserve(found.size());
  for (const Nearby& close : found) {
    entities.push_back(close.entity);
  }
  return entities;
}

bool handle_local_world_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      integration::script_operation(*request).rfind("localWorld", 0) != 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (requests.size() < kMaxQueued) {
    requests.push_back(Request{id->as_int(), *request});
  }
  return true;
}

void answer_local_world(bool in_world) {
  std::vector<Request> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(requests);
  }
  if (taken.empty()) {
    return;
  }
  std::vector<Reply> answered;
  for (const Request& request : taken) {
    answered.push_back(Reply{request.id, answer(request.body, in_world)});
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (Reply& reply : answered) {
    replies.push_back(std::move(reply));
  }
}

void pump_local_world(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<Reply> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (const Reply& reply : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(reply.id)},
                                    {"value", reply.value}}));
  }
}

}  // namespace kcds::runtime::physics
