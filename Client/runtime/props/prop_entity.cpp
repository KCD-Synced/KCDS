#include "prop_entity.hpp"

#include <intrin.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

#include "pose_math.hpp"

namespace kcds::runtime::props::entity {
namespace {

// IEntitySystem (byte offsets).
constexpr std::size_t kClassRegistry = 88;
constexpr std::size_t kFindClass = 24;
constexpr std::size_t kSpawnEntity = 96;
constexpr std::size_t kInitEntity = 104;
constexpr std::size_t kGetEntity = 112;
constexpr std::size_t kFindByName = 128;
constexpr std::size_t kRemoveEntity = 152;
// IEntitySystem slot 22, its entity iterator (slots: AddRef 1, Release 2, Next 4, MoveFirst 6),
// and the class of an entity (CEntity slot 3), as System.GetEntitiesByClass walks them.
constexpr std::size_t kIteratorSlot = 22;
constexpr std::size_t kIteratorAddRef = 1;
constexpr std::size_t kIteratorRelease = 2;
constexpr std::size_t kIteratorNext = 4;
constexpr std::size_t kIteratorMoveFirst = 6;
constexpr std::size_t kEntityClass = 3;
// ICharacterInstance: its default skeleton and skeleton pose; IDefaultSkeleton GetJointIDByName;
// ISkeletonPose GetAbsJointByID (as the entity binding's GetBonePos).
constexpr std::size_t kDefaultSkeleton = 11;
constexpr std::size_t kSkeletonPose = 7;
constexpr std::size_t kJointIdByName = 7;
constexpr std::size_t kAbsoluteJoint = 27;
// IDefaultSkeleton GetJointNameByID; CDefaultSkeleton's joints (their count in the int before the
// first), as animation/female_retarget.cpp reads them.
constexpr std::size_t kJointNameSlot = 6;
// IDefaultSkeleton: GetJointCount and GetJointParentIDByID (CDefaultSkeleton).
constexpr std::size_t kJointCountSlot = 1;
constexpr std::size_t kJointParentSlot = 2;
constexpr std::size_t kSkeletonJoints = 8;
constexpr int kMaxJoints = 1024;
// SEntitySpawnParams and the flags the script SpawnEntity adds.
constexpr std::size_t kSpawnParamsSize = 144;
constexpr std::size_t kSpawnGuid = 8;
constexpr std::size_t kSpawnClass = 24;
constexpr std::size_t kSpawnName = 56;
constexpr std::size_t kSpawnFlags = 64;
constexpr std::size_t kSpawnFlagsExtended = 68;
constexpr std::size_t kSpawnPosition = 80;
constexpr std::size_t kSpawnRotation = 92;
constexpr std::uint32_t kSpawnScriptFlags = 0x1000000;
constexpr std::uint32_t kSpawnScriptFlagsExtended = 0x400000;
constexpr char kPropClass[] = "BasicEntity";
// CEntity: its id and vtable slots.
constexpr std::size_t kEntityId = 12;
// Its name, a pointer to the characters of a string, as the script binding's GetName reads it
constexpr std::size_t kEntityName = 224;
constexpr std::size_t kSetPos = 39;
constexpr std::size_t kSetRotation = 41;
constexpr std::size_t kGetWorldTM = 32;
constexpr std::size_t kSetWorldTM = 30;
// GetWorldBounds(AABB&): the local box (slot 35) through the world matrix; an
// entity without anything to bound gets a box of +-1e15.
constexpr std::size_t kGetWorldBounds = 34;
// CEntity::GetLocalBounds (slot 35): the box around the entity in its own
// frame; GetWorldBounds (slot 34) calls it and turns the box by the world matrix.
constexpr std::size_t kGetLocalBounds = 35;
constexpr std::size_t kSetMaterial = 84;
constexpr std::size_t kGetCharacter = 101;
constexpr std::size_t kLoadGeometry = 108;
constexpr std::size_t kLoadCharacter = 109;
constexpr std::size_t kGetStatObj = 103;
constexpr std::size_t kStatObjFilePath = 41;
constexpr std::size_t kGetMaterial = 85;
constexpr std::size_t kMaterialName = 8;
constexpr int kMeshSlot = 0;
constexpr std::size_t kHide = 63;
constexpr std::size_t kIsHidden = 64;
constexpr int kCharacterSlot = 0;
// CEntity GetProxy(type) (slot 74): the render proxy (type 0) SetOpacity, as Entity:RenderParams;
// the script proxy (type 2) and its GetScriptTable (npcs/npc_body.cpp's chain).
constexpr std::size_t kGetProxy = 74;
constexpr int kRenderProxy = 0;
constexpr int kPhysicsProxy = 1;
constexpr std::size_t kSetOpacity = 288 / 8;
constexpr int kScriptProxy = 2;
constexpr std::size_t kScriptTable = 18;
// IScriptTable and ScriptAnyValue: its type (4 a number, a float at + 8; 5 a string; 6 a table)
// and the value at + 8, as the script binding writes them.
constexpr std::size_t kReleaseTable = 4;
constexpr std::size_t kSetValueAny = 7;
constexpr std::size_t kGetValueAny = 8;
constexpr int kAnyBoolean = 2;
constexpr int kAnyNumber = 4;
constexpr int kAnyString = 5;
constexpr int kAnyTable = 6;
constexpr std::size_t kAnyValue = 8;
constexpr std::size_t kAnySize = 32;
// The physics component's EnablePhysics and Physicalize (slot 20, the call the entity binding's
// Physicalize makes).
constexpr std::size_t kEnablePhysics = 24;
constexpr std::size_t kPhysicalize = 20;
// SEntityPhysicalizeParams (112 bytes, built): the physics type (1 static, 2 rigid),
// the geometry slot and the mass in kilograms (-1 leaves it to the density).
constexpr std::size_t kPhysicalizeParamsSize = 128;
constexpr std::size_t kPhysicalizeType = 0;
constexpr std::size_t kPhysicalizeSlot = 4;
constexpr std::size_t kPhysicalizeMass = 12;
constexpr std::int32_t kPhysicsNone = 0;
constexpr std::int32_t kPhysicsStatic = 1;
constexpr std::int32_t kPhysicsRigid = 2;
constexpr std::int32_t kPhysicsSoft = 8;
// I3DEngine: the material manager; its CreateMaterial and LoadMaterial.
constexpr std::size_t kMaterialManager = 197;
constexpr std::size_t kCreateMaterial = 3;
constexpr std::size_t kLoadMaterial = 7;
// ICharacterInstance: its skeleton animation and animation set; GetAnimIDByName.
constexpr std::size_t kSkeletonAnim = 5;
constexpr std::size_t kAnimationSet = 16;
constexpr std::size_t kAnimIdByName = 6;
// ISkeletonAnim.
constexpr std::size_t kStartAnimationById = 9;
constexpr std::size_t kStopAnimationInLayer = 10;
constexpr std::size_t kLayerPlaybackSpeed = 23;
// SetLayerNormalizedTime(layer, time 0..1), as the entity binding's SetAnimationTime.
constexpr std::size_t kSetLayerNormalizedTime = 29;
constexpr std::uint32_t kLayer = 0;
// CryCharAnimationParams flags: loop, or hold the last key.
constexpr std::uint32_t kLoopAnimation = 2;
constexpr std::uint32_t kRepeatLastKey = 4;

// The animation parameters as the entity binding's StartAnimation fills them.
struct AnimationParams {
  float transition;
  float key_time;
  float speed;
  float weight;
  std::uint32_t layer;
  float layer_weight;
  std::uint32_t flags;
  std::uint32_t user_token;
  float user_data[8];
};
static_assert(sizeof(AnimationParams) == 64);

// The physics component getter returns a std::shared_ptr: the pointer and its control block.
struct SharedPointer {
  void* object;
  void* control;
};

using Get = void*(__fastcall*)(void* object);
using FindClass = void*(__fastcall*)(void* registry, const char* name);
using FindByName = void*(__fastcall*)(void* entities, const char* name);
using SpawnParamsInit = void*(__fastcall*)(void* params);
using SpawnEntity = void*(__fastcall*)(void* entities, void* params, bool init);
using InitEntity = bool(__fastcall*)(void* entities, void* entity, void* params);
using GetProxy = void*(__fastcall*)(void* entity, int type);
using ReleaseTable = void(__fastcall*)(void* table);
using SetValueAny = void(__fastcall*)(void* table, const char* key, const void* value, bool chain);
using GetValueAny = bool(__fastcall*)(void* table, const char* key, void* value, bool chain);
using RemoveEntity = void(__fastcall*)(void* entities, std::uint32_t id, bool now);
using SetPos = void(__fastcall*)(void* entity, const float* position, int flags, bool physics,
                                 bool force);
using SetRotation = void(__fastcall*)(void* entity, const float* rotation, int flags);
using GetWorldTM = const float*(__fastcall*)(void* entity);
using SetMaterial = void(__fastcall*)(void* entity, void* material);
using GetCharacter = void*(__fastcall*)(void* entity, int slot);
using LoadCharacter = int(__fastcall*)(void* entity, int slot, const char* file, int flags);
using Hide = void(__fastcall*)(void* entity, bool hidden);
using LoadGeometry = int(__fastcall*)(void* entity, int slot, const char* file,
                                      const char* geometry, int flags);
using PhysicsComponent = SharedPointer*(__fastcall*)(void* entity, SharedPointer* out, int create);
using EnablePhysics = void(__fastcall*)(void* component, bool enable);
using Physicalize = void(__fastcall*)(void* component, const void* params);
using PhysicalizeParamsInit = void*(__fastcall*)(void* params);
using SetScale = void(__fastcall*)(void* entity, const float* scale, int flags);
using LoadMaterial = void*(__fastcall*)(void* manager, const char* name, bool make, bool fixed,
                                        unsigned flags, bool load);
using CreateMaterial = void*(__fastcall*)(void* manager, const char* name, int flags);
using AnimIdByName = int(__fastcall*)(void* animations, const char* name);
using StartAnimationById = bool(__fastcall*)(void* skeleton, int id, const AnimationParams* params);
using StopAnimationInLayer = bool(__fastcall*)(void* skeleton, std::uint32_t layer, float blend);
using LayerPlaybackSpeed = void(__fastcall*)(void* skeleton, std::uint32_t layer, float speed);
using SetLayerNormalizedTime = void(__fastcall*)(void* skeleton, std::uint32_t layer, float time);

Natives natives;
PlacementNatives placement;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

template <class Fn>
Fn at_offset(void* object, std::size_t offset) {
  return slot<Fn>(object, offset / sizeof(void*));
}

void* entities() {
  return natives.entity_system ? natives.entity_system() : nullptr;
}

// Releases a shared_ptr the game handed out (its control block's use count at + 8).
void release(const SharedPointer& pointer) {
  if (!pointer.control) {
    return;
  }
  auto counts = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(pointer.control) + 8);
  if (_InterlockedExchangeAdd(counts, -1) == 1) {
    void** vtable = *static_cast<void***>(pointer.control);
    reinterpret_cast<void(__fastcall*)(void*)>(vtable[0])(pointer.control);
    if (_InterlockedExchangeAdd(counts + 1, -1) == 1) {
      reinterpret_cast<void(__fastcall*)(void*)>(vtable[1])(pointer.control);
    }
  }
}

// A prop must not collide: its collider pushed the replica away from the owner's position every
// frame (shaking legs, the document off the hands, live).
void physics_off(void* entity) {
  if (!natives.physics_component) {
    return;
  }
  SharedPointer component{};
  reinterpret_cast<PhysicsComponent>(natives.physics_component)(entity, &component, 1);
  if (component.object) {
    slot<EnablePhysics>(component.object, kEnablePhysics)(component.object, false);
  }
  release(component);
}

void* material_named(const char* name) {
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  void* manager = engine ? slot<Get>(engine, kMaterialManager)(engine) : nullptr;
  return manager ? slot<LoadMaterial>(manager, kLoadMaterial)(manager, name, true, false, 0, true)
                 : nullptr;
}

void set_material(void* entity, const char* name) {
  void* material = material_named(name);
  if (material) {
    slot<SetMaterial>(entity, kSetMaterial)(entity, material);
  }
}

// CryEngine's Quat::CreateRotationXYZ (the game's own): x, y, z, w.
void rotation_xyz(const float angles[3], float quat[4]) {
  const float sx = std::sin(angles[0] * 0.5f);
  const float cx = std::cos(angles[0] * 0.5f);
  const float sy = std::sin(angles[1] * 0.5f);
  const float cy = std::cos(angles[1] * 0.5f);
  const float sz = std::sin(angles[2] * 0.5f);
  const float cz = std::cos(angles[2] * 0.5f);
  quat[0] = cz * cy * sx - sz * sy * cx;
  quat[1] = cz * sy * cx + sz * cy * sx;
  quat[2] = sz * cy * cx - cz * sy * sx;
  quat[3] = cx * cy * cz + sx * sy * sz;
}

void* skeleton_of(void* entity, void** animations) {
  void* character = slot<GetCharacter>(entity, kGetCharacter)(entity, kCharacterSlot);
  if (!character) {
    return nullptr;
  }
  if (animations) {
    *animations = slot<Get>(character, kAnimationSet)(character);
  }
  return slot<Get>(character, kSkeletonAnim)(character);
}

}  // namespace

void draw_with_material(void* entity, const std::string& material) {
  if (entity && !material.empty()) {
    set_material(entity, material.c_str());
  }
}

void* load_material(const std::string& material) {
  return material.empty() ? nullptr : material_named(material.c_str());
}

void* create_material(const char* name, int flags) {
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  void* manager = engine ? slot<Get>(engine, kMaterialManager)(engine) : nullptr;
  return manager ? slot<CreateMaterial>(manager, kCreateMaterial)(manager, name, flags) : nullptr;
}

void install(const Natives& given) {
  natives = given;
}

void* skeleton_anim_of(void* entity) {
  return entity ? skeleton_of(entity, nullptr) : nullptr;
}

void install_placement(const PlacementNatives& given) {
  placement = given;
}

void* find(const std::string& name) {
  void* system = entities();
  return system ? at_offset<FindByName>(system, kFindByName)(system, name.c_str()) : nullptr;
}

std::string name_of(void* entity, std::size_t max_length) {
  const char* text = nullptr;
  if (entity) {
    std::memcpy(&text, static_cast<const std::uint8_t*>(entity) + kEntityName, sizeof(text));
  }
  return text ? std::string(text, strnlen(text, max_length)) : std::string();
}

void* spawn(const std::string& name, const float position[3], const char* model,
            const char* material) {
  void* prop = spawn_class(name, kPropClass, position);
  if (!prop) {
    return nullptr;
  }
  slot<LoadCharacter>(prop, kLoadCharacter)(prop, kCharacterSlot, model, 0);
  if (material && material[0] != '\0') {
    set_material(prop, material);
  }
  physics_off(prop);
  return prop;
}

namespace {

// Spawns a BasicEntity showing the mesh with its physics as the game made it.
void* spawn_mesh_entity(const std::string& name, const float position[3], const std::string& model,
                        const std::string& material) {
  const std::size_t dot = model.rfind('.');
  const std::string extension = dot == std::string::npos ? std::string() : model.substr(dot + 1);
  const bool character =
      _stricmp(extension.c_str(), "cdf") == 0 || _stricmp(extension.c_str(), "chr") == 0;
  const bool mesh = _stricmp(extension.c_str(), "cgf") == 0;
  if (!character && !mesh) {
    return nullptr;
  }
  void* prop = spawn_class(name, kPropClass, position);
  if (!prop) {
    return nullptr;
  }
  const int loaded =
      character ? slot<LoadCharacter>(prop, kLoadCharacter)(prop, kCharacterSlot, model.c_str(), 0)
                : slot<LoadGeometry>(prop, kLoadGeometry)(prop, kCharacterSlot, model.c_str(),
                                                          nullptr, 0);
  if (loaded < 0) {
    remove(name);
    return nullptr;
  }
  if (!material.empty()) {
    set_material(prop, material.c_str());
  }
  return prop;
}

}  // namespace

void* spawn_mesh(const std::string& name, const float position[3], const std::string& model,
                 const std::string& material) {
  void* prop = spawn_mesh_entity(name, position, model, material);
  if (prop) {
    physics_off(prop);
  }
  return prop;
}

void* spawn_solid_mesh(const std::string& name, const float position[3], const std::string& model) {
  return spawn_mesh_entity(name, position, model, std::string());
}

void set_scale(void* entity, const float scale[3]) {
  if (entity && placement.set_scale) {
    reinterpret_cast<SetScale>(placement.set_scale)(entity, scale, 0);
  }
}

void set_opacity(void* entity, float opacity) {
  if (!entity) {
    return;
  }
  using GetProxy = void*(__fastcall*)(void* entity, int type);
  using SetOpacity = void(__fastcall*)(void* proxy, float opacity);
  void* proxy = slot<GetProxy>(entity, kGetProxy)(entity, kRenderProxy);
  if (proxy) {
    slot<SetOpacity>(proxy, kSetOpacity)(proxy, std::fmin(1.0f, std::fmax(0.0f, opacity)));
  }
}

bool physicalize(void* entity, Physics physics, float mass) {
  if (!entity || !natives.physics_component || !placement.physics_params_init) {
    return false;
  }
  alignas(16) std::uint8_t params[kPhysicalizeParamsSize];
  reinterpret_cast<PhysicalizeParamsInit>(placement.physics_params_init)(params);
  std::int32_t type = kPhysicsNone;
  if (physics == Physics::Rigid) {
    type = kPhysicsRigid;
  } else if (physics == Physics::Static) {
    type = kPhysicsStatic;
  } else if (physics == Physics::Soft) {
    type = kPhysicsSoft;
  }
  const std::int32_t slot_index = kCharacterSlot;
  std::memcpy(params + kPhysicalizeType, &type, sizeof(type));
  std::memcpy(params + kPhysicalizeSlot, &slot_index, sizeof(slot_index));
  if (physics == Physics::Rigid || physics == Physics::Soft) {
    std::memcpy(params + kPhysicalizeMass, &mass, sizeof(mass));
  }
  // As the entity binding's Physicalize does: the entity's own physics proxy, made
  // first when it has none (a second component would leave the entity its old body).
  using GetProxy = void*(__fastcall*)(void* entity, int type);
  void* proxy = slot<GetProxy>(entity, kGetProxy)(entity, kPhysicsProxy);
  if (!proxy && physics == Physics::None) {
    return false;
  }
  if (!proxy) {
    SharedPointer component{};
    reinterpret_cast<PhysicsComponent>(natives.physics_component)(entity, &component, 1);
    release(component);
    proxy = slot<GetProxy>(entity, kGetProxy)(entity, kPhysicsProxy);
  }
  if (proxy) {
    slot<Physicalize>(proxy, kPhysicalize)(proxy, params);
  }
  return proxy != nullptr;
}

namespace {

// Fills the spawn parameters of an entity of the class, named so, at the world position, as the
// script SpawnEntity does; false when the class is unknown or a native is missing. `params` holds
// the name's characters by pointer, so `name` must outlive the spawn.
bool spawn_params(void* system, const std::string& name, const char* entity_class,
                  const float position[3], std::uint8_t* params) {
  if (!system || !natives.spawn_params_init) {
    return false;
  }
  void* registry = at_offset<Get>(system, kClassRegistry)(system);
  void* klass =
      registry ? at_offset<FindClass>(registry, kFindClass)(registry, entity_class) : nullptr;
  if (!klass) {
    return false;
  }
  reinterpret_cast<SpawnParamsInit>(natives.spawn_params_init)(params);
  const char* name_text = name.c_str();
  std::memcpy(params + kSpawnClass, &klass, sizeof(klass));
  std::memcpy(params + kSpawnName, &name_text, sizeof(name_text));
  std::uint32_t flags = 0;
  std::memcpy(&flags, params + kSpawnFlags, sizeof(flags));
  flags |= kSpawnScriptFlags;
  std::memcpy(params + kSpawnFlags, &flags, sizeof(flags));
  std::memcpy(&flags, params + kSpawnFlagsExtended, sizeof(flags));
  flags |= kSpawnScriptFlagsExtended;
  std::memcpy(params + kSpawnFlagsExtended, &flags, sizeof(flags));
  std::memcpy(params + kSpawnPosition, position, 3 * sizeof(float));
  return true;
}

bool set_script_flag_impl(void* entity, const char* key, bool flag) {
  void* proxy = slot<GetProxy>(entity, kGetProxy)(entity, kScriptProxy);
  void* script = proxy ? slot<Get>(proxy, kScriptTable)(proxy) : nullptr;
  if (!script) {
    return false;
  }
  alignas(16) std::uint8_t value[kAnySize] = {};
  std::memcpy(value, &kAnyBoolean, sizeof(kAnyBoolean));
  value[kAnyValue] = flag ? 1 : 0;
  slot<SetValueAny>(script, kSetValueAny)(script, key, value, false);
  return true;
}

// The table of that key in a script table (a reference added for the caller), or null.
void* table_in(void* table, const char* key) {
  alignas(16) std::uint8_t holder[kAnySize] = {};
  if (!slot<GetValueAny>(table, kGetValueAny)(table, key, holder, false)) {
    return nullptr;
  }
  int type = 0;
  void* found = nullptr;
  std::memcpy(&type, holder, sizeof(type));
  std::memcpy(&found, holder + kAnyValue, sizeof(found));
  return type == kAnyTable ? found : nullptr;
}

// Writes the values into the entity script's Properties table; false when it has none.
bool set_properties(void* entity, const std::vector<ScriptProperty>& properties) {
  void* proxy = slot<GetProxy>(entity, kGetProxy)(entity, kScriptProxy);
  void* script = proxy ? slot<Get>(proxy, kScriptTable)(proxy) : nullptr;
  if (!script) {
    return false;
  }
  alignas(16) std::uint8_t holder[kAnySize] = {};
  if (!slot<GetValueAny>(script, kGetValueAny)(script, "Properties", holder, false)) {
    return false;
  }
  int type = 0;
  void* table = nullptr;
  std::memcpy(&type, holder, sizeof(type));
  std::memcpy(&table, holder + kAnyValue, sizeof(table));
  if (type != kAnyTable || !table) {
    return false;
  }
  for (const ScriptProperty& property : properties) {
    void* target = property.group ? table_in(table, property.group) : table;
    if (!target) {
      continue;
    }
    alignas(16) std::uint8_t value[kAnySize] = {};
    if (property.text) {
      std::memcpy(value, &kAnyString, sizeof(kAnyString));
      std::memcpy(value + kAnyValue, &property.text, sizeof(property.text));
    } else if (property.boolean) {
      std::memcpy(value, &kAnyBoolean, sizeof(kAnyBoolean));
      value[kAnyValue] = property.number != 0.0f ? 1 : 0;
    } else {
      std::memcpy(value, &kAnyNumber, sizeof(kAnyNumber));
      std::memcpy(value + kAnyValue, &property.number, sizeof(property.number));
    }
    slot<SetValueAny>(target, kSetValueAny)(target, property.key, value, false);
    if (target != table) {
      slot<ReleaseTable>(target, kReleaseTable)(target);
    }
  }
  // The table the value held is the script system's; it was added a reference for us.
  slot<ReleaseTable>(table, kReleaseTable)(table);
  return true;
}

}  // namespace

void* spawn_class(const std::string& name, const char* entity_class, const float position[3]) {
  void* system = entities();
  alignas(16) std::uint8_t params[kSpawnParamsSize];
  if (!spawn_params(system, name, entity_class, position, params)) {
    return nullptr;
  }
  return at_offset<SpawnEntity>(system, kSpawnEntity)(system, params, true);
}

void* spawn_class_with(const std::string& name, const char* entity_class, const float position[3],
                       const float rotation[4], const std::vector<ScriptProperty>& properties,
                       std::uint64_t guid) {
  void* system = entities();
  alignas(16) std::uint8_t params[kSpawnParamsSize];
  if (!spawn_params(system, name, entity_class, position, params)) {
    return nullptr;
  }
  std::memcpy(params + kSpawnRotation, rotation, 4 * sizeof(float));
  std::memcpy(params + kSpawnGuid, &guid, sizeof(guid));
  void* entity = at_offset<SpawnEntity>(system, kSpawnEntity)(system, params, false);
  if (!entity) {
    return nullptr;
  }
  // Without its Properties table the entity is initialised with the class's own values.
  set_properties(entity, properties);
  if (!at_offset<InitEntity>(system, kInitEntity)(system, entity, params)) {
    return nullptr;
  }
  return entity;
}

void remove(const std::string& name) {
  void* system = entities();
  void* prop = find(name);
  if (!system || !prop) {
    return;
  }
  std::uint32_t id = 0;
  std::memcpy(&id, static_cast<std::uint8_t*>(prop) + kEntityId, sizeof(id));
  at_offset<RemoveEntity>(system, kRemoveEntity)(system, id, false);
}

void world_position(void* entity, float position[3]) {
  const float* matrix = slot<GetWorldTM>(entity, kGetWorldTM)(entity);
  position[0] = matrix[3];
  position[1] = matrix[7];
  position[2] = matrix[11];
}

void world_pose(void* entity, float position[3], float rotation[4]) {
  const float* matrix = slot<GetWorldTM>(entity, kGetWorldTM)(entity);
  position[0] = matrix[3];
  position[1] = matrix[7];
  position[2] = matrix[11];
  pose_math::from_matrix(matrix, rotation);
  pose_math::normalize(rotation);
}

bool world_bounds(void* entity, float min[3], float max[3]) {
  using GetWorldBounds = void(__fastcall*)(void* entity, float* box);
  if (!entity) {
    return false;
  }
  float box[6] = {};
  slot<GetWorldBounds>(entity, kGetWorldBounds)(entity, box);
  for (int k = 0; k < 3; ++k) {
    const bool usable = std::isfinite(box[k]) && std::isfinite(box[k + 3]) &&
                        std::fabs(box[k]) < 1e7f && std::fabs(box[k + 3]) < 1e7f &&
                        box[k] <= box[k + 3];
    if (!usable) {
      return false;
    }
    min[k] = box[k];
    max[k] = box[k + 3];
  }
  return true;
}

bool local_bounds(void* entity, float min[3], float max[3]) {
  using GetLocalBounds = void(__fastcall*)(void* entity, float* box);
  if (!entity) {
    return false;
  }
  float box[6] = {};
  slot<GetLocalBounds>(entity, kGetLocalBounds)(entity, box);
  for (int k = 0; k < 3; ++k) {
    const bool usable = std::isfinite(box[k]) && std::isfinite(box[k + 3]) &&
                        std::fabs(box[k]) < 1e5f && std::fabs(box[k + 3]) < 1e5f &&
                        box[k] <= box[k + 3];
    if (!usable) {
      return false;
    }
    min[k] = box[k];
    max[k] = box[k + 3];
  }
  return true;
}

float world_scale(void* entity) {
  const float* matrix = slot<GetWorldTM>(entity, kGetWorldTM)(entity);
  return std::sqrt(matrix[0] * matrix[0] + matrix[4] * matrix[4] + matrix[8] * matrix[8]);
}

std::string mesh_file(void* entity, std::size_t max_length) {
  using GetStatObj = void*(__fastcall*)(void* entity, int slot);
  using GetFilePath = const char*(__fastcall*)(void* mesh);
  if (!entity) {
    return {};
  }
  void* mesh = slot<GetStatObj>(entity, kGetStatObj)(entity, kMeshSlot);
  if (!mesh) {
    return {};
  }
  const char* path = slot<GetFilePath>(mesh, kStatObjFilePath)(mesh);
  return path ? std::string(path, strnlen(path, max_length)) : std::string();
}

bool show_mesh(void* entity, const std::string& model) {
  if (!entity) {
    return false;
  }
  const int loaded =
      slot<LoadGeometry>(entity, kLoadGeometry)(entity, kCharacterSlot, model.c_str(), nullptr, 0);
  return loaded >= 0;
}

std::string material_name(void* entity, std::size_t max_length) {
  using GetMaterial = void*(__fastcall*)(void* entity);
  using GetName = const char*(__fastcall*)(void* material);
  if (!entity) {
    return {};
  }
  void* material = slot<GetMaterial>(entity, kGetMaterial)(entity);
  if (!material) {
    return {};
  }
  const char* name = slot<GetName>(material, kMaterialName)(material);
  return name ? std::string(name, strnlen(name, max_length)) : std::string();
}

bool joint_pose(void* entity, const char* joint, float position[3], float rotation[4]) {
  using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
  using AbsoluteJoint = const float*(__fastcall*)(void* pose, int id);
  if (!entity || !joint) {
    return false;
  }
  void* character = slot<GetCharacter>(entity, kGetCharacter)(entity, kCharacterSlot);
  if (!character) {
    return false;
  }
  void* skeleton = slot<Get>(character, kDefaultSkeleton)(character);
  void* pose = slot<Get>(character, kSkeletonPose)(character);
  if (!skeleton || !pose) {
    return false;
  }
  const std::int16_t id = slot<JointId>(skeleton, kJointIdByName)(skeleton, joint);
  if (id < 0) {
    return false;
  }
  const float* local = slot<AbsoluteJoint>(pose, kAbsoluteJoint)(pose, id);
  if (!local) {
    return false;
  }
  // QuatT: the turn (x, y, z, w), then the position, in the character's space.
  float body_position[3];
  float body_rotation[4];
  world_pose(entity, body_position, body_rotation);
  float joint_rotation[4] = {local[0], local[1], local[2], local[3]};
  pose_math::normalize(joint_rotation);
  pose_math::compose(body_position, body_rotation, local + 4, joint_rotation, position, rotation);
  return true;
}

namespace {

// The entity's character's default skeleton, or null.
void* default_skeleton(void* entity) {
  void* character =
      entity ? slot<GetCharacter>(entity, kGetCharacter)(entity, kCharacterSlot) : nullptr;
  return character ? slot<Get>(character, kDefaultSkeleton)(character) : nullptr;
}

}  // namespace

bool has_joint(void* entity, const char* joint) {
  using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
  void* skeleton = joint ? default_skeleton(entity) : nullptr;
  return skeleton && slot<JointId>(skeleton, kJointIdByName)(skeleton, joint) >= 0;
}

int joint_id(void* entity, const char* joint) {
  using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
  void* skeleton = joint ? default_skeleton(entity) : nullptr;
  return skeleton ? int(slot<JointId>(skeleton, kJointIdByName)(skeleton, joint)) : -1;
}

std::string body_joint_name(void* entity, const std::string& wanted) {
  if (wanted.empty() || has_joint(entity, wanted.c_str())) {
    return wanted.empty() ? std::string() : wanted;
  }
  std::string name = wanted;
  for (const char* prefix : {"Bip01 ", "Bip01_", "Bip01"}) {
    if (name.rfind(prefix, 0) == 0) {
      name.erase(0, std::strlen(prefix));
      break;
    }
  }
  if (name.rfind("R ", 0) == 0) {
    name = "Right" + name.substr(2);
  } else if (name.rfind("L ", 0) == 0) {
    name = "Left" + name.substr(2);
  }
  name.erase(std::remove(name.begin(), name.end(), ' '), name.end());
  return name != wanted && has_joint(entity, name.c_str()) ? name : std::string();
}

std::vector<std::string> joint_names(void* entity) {
  using JointName = const char*(__fastcall*)(void* skeleton, std::int16_t id);
  std::vector<std::string> names;
  void* skeleton = default_skeleton(entity);
  if (!skeleton) {
    return names;
  }
  const char* joints =
      *reinterpret_cast<const char* const*>(static_cast<char*>(skeleton) + kSkeletonJoints);
  const int count = joints ? *reinterpret_cast<const int*>(joints - sizeof(int)) : 0;
  if (count <= 0 || count > kMaxJoints) {
    return names;
  }
  for (int id = 0; id < count; ++id) {
    const char* name = slot<JointName>(skeleton, kJointNameSlot)(skeleton, std::int16_t(id));
    names.emplace_back(name ? name : "");
  }
  return names;
}

int joint_count(void* entity) {
  using JointCount = std::uint32_t(__fastcall*)(void* skeleton);
  void* skeleton = default_skeleton(entity);
  if (!skeleton) {
    return 0;
  }
  const std::uint32_t count = slot<JointCount>(skeleton, kJointCountSlot)(skeleton);
  return count > std::uint32_t(kMaxJoints) ? 0 : int(count);
}

int joint_parent(void* entity, int id) {
  using JointParent = std::int32_t(__fastcall*)(void* skeleton, std::int16_t id);
  void* skeleton = default_skeleton(entity);
  if (!skeleton || id < 0 || id >= kMaxJoints) {
    return -1;
  }
  const std::int32_t parent =
      slot<JointParent>(skeleton, kJointParentSlot)(skeleton, std::int16_t(id));
  // The record keeps the parent as an unsigned 16-bit index: 0xFFFF is none.
  return parent < 0 || parent >= 0xFFFF ? -1 : parent;
}

bool joint_pose_by_id(void* entity, int id, float position[3], float rotation[4]) {
  using AbsoluteJoint = const float*(__fastcall*)(void* pose, int id);
  if (!entity || id < 0) {
    return false;
  }
  void* character = slot<GetCharacter>(entity, kGetCharacter)(entity, kCharacterSlot);
  void* pose = character ? slot<Get>(character, kSkeletonPose)(character) : nullptr;
  if (!pose || id >= joint_count(entity)) {
    return false;
  }
  const float* local = slot<AbsoluteJoint>(pose, kAbsoluteJoint)(pose, id);
  if (!local) {
    return false;
  }
  float body_position[3];
  float body_rotation[4];
  world_pose(entity, body_position, body_rotation);
  float joint_rotation[4] = {local[0], local[1], local[2], local[3]};
  pose_math::normalize(joint_rotation);
  pose_math::compose(body_position, body_rotation, local + 4, joint_rotation, position, rotation);
  return true;
}

bool joint_places(void* entity, int count, float* positions, std::vector<bool>& known) {
  using AbsoluteJoint = const float*(__fastcall*)(void* pose, int id);
  known.assign(std::size_t(std::max(count, 0)), false);
  void* body = character(entity);
  void* pose = body ? slot<Get>(body, kSkeletonPose)(body) : nullptr;
  if (!pose || count <= 0) {
    return false;
  }
  float body_position[3];
  float body_rotation[4];
  world_pose(entity, body_position, body_rotation);
  const auto absolute = slot<AbsoluteJoint>(pose, kAbsoluteJoint);
  for (int id = 0; id < count; ++id) {
    const float* local = absolute(pose, id);
    if (!local) {
      continue;
    }
    float joint_rotation[4] = {local[0], local[1], local[2], local[3]};
    float rotation[4];
    pose_math::normalize(joint_rotation);
    pose_math::compose(body_position, body_rotation, local + 4, joint_rotation,
                       &positions[std::size_t(id) * 3], rotation);
    known[id] = true;
  }
  return true;
}

void* character(void* entity) {
  return entity ? slot<GetCharacter>(entity, kGetCharacter)(entity, kCharacterSlot) : nullptr;
}

bool default_joint(void* entity, const char* joint, int which, float position[3],
                   float rotation[4]) {
  using JointId = std::int16_t(__fastcall*)(void* skeleton, const char* name);
  using DefaultJoint = const float*(__fastcall*)(void* skeleton, std::int16_t id);
  if (!entity || !joint) {
    return false;
  }
  void* character = slot<GetCharacter>(entity, kGetCharacter)(entity, kCharacterSlot);
  void* skeleton = character ? slot<Get>(character, kDefaultSkeleton)(character) : nullptr;
  if (!skeleton) {
    return false;
  }
  const std::int16_t id = slot<JointId>(skeleton, kJointIdByName)(skeleton, joint);
  if (id < 0) {
    return false;
  }
  const float* pose = slot<DefaultJoint>(skeleton, std::size_t(which))(skeleton, id);
  if (!pose) {
    return false;
  }
  for (int i = 0; i < 4; ++i) {
    rotation[i] = pose[i];
  }
  for (int i = 0; i < 3; ++i) {
    position[i] = pose[4 + i];
  }
  return true;
}

bool helper_pose(void* entity, const char* helper, float position[3], float rotation[4]) {
  using GetStatObj = void*(__fastcall*)(void* entity, int slot);
  using GetHelperTM = const float*(__fastcall*)(void* model, const char* name);
  constexpr std::size_t kGetStatObj = 103;
  constexpr std::size_t kGetHelperTM = 47;
  if (!entity || !helper) {
    return false;
  }
  void* model = slot<GetStatObj>(entity, kGetStatObj)(entity, 0);
  const float* matrix = model ? slot<GetHelperTM>(model, kGetHelperTM)(model, helper) : nullptr;
  if (!matrix) {
    return false;
  }
  pose_math::from_matrix(matrix, rotation);
  position[0] = matrix[3];
  position[1] = matrix[7];
  position[2] = matrix[11];
  return true;
}

void* by_id(std::uint32_t id) {
  using GetEntity = void*(__fastcall*)(void* entities, std::uint32_t id);
  void* system = entities();
  if (!system || id == 0) {
    return nullptr;
  }
  return at_offset<GetEntity>(system, kGetEntity)(system, id);
}

std::uint32_t id_of(void* entity) {
  std::uint32_t id = 0;
  if (entity) {
    std::memcpy(&id, static_cast<std::uint8_t*>(entity) + kEntityId, sizeof(id));
  }
  return id;
}

bool set_script_flag(void* entity, const char* key, bool value) {
  return entity && key && set_script_flag_impl(entity, key, value);
}

bool script_number(void* entity, const char* key, float& value) {
  if (!entity || !key) {
    return false;
  }
  void* proxy = slot<GetProxy>(entity, kGetProxy)(entity, kScriptProxy);
  void* script = proxy ? slot<Get>(proxy, kScriptTable)(proxy) : nullptr;
  if (!script) {
    return false;
  }
  alignas(16) std::uint8_t holder[kAnySize] = {};
  if (!slot<GetValueAny>(script, kGetValueAny)(script, key, holder, false)) {
    return false;
  }
  int type = 0;
  std::memcpy(&type, holder, sizeof(type));
  if (type != kAnyNumber) {
    return false;
  }
  std::memcpy(&value, holder + kAnyValue, sizeof(value));
  return true;
}

bool set_script_number(void* entity, const char* key, float value) {
  if (!entity || !key) {
    return false;
  }
  void* proxy = slot<GetProxy>(entity, kGetProxy)(entity, kScriptProxy);
  void* script = proxy ? slot<Get>(proxy, kScriptTable)(proxy) : nullptr;
  if (!script) {
    return false;
  }
  alignas(16) std::uint8_t holder[kAnySize] = {};
  std::memcpy(holder, &kAnyNumber, sizeof(kAnyNumber));
  std::memcpy(holder + kAnyValue, &value, sizeof(value));
  slot<SetValueAny>(script, kSetValueAny)(script, key, holder, false);
  return true;
}

void each_entity(const std::function<void(void* entity)>& visit) {
  using Call = void(__fastcall*)(void*);
  void* system = entities();
  void* iterator = system ? slot<Get>(system, kIteratorSlot)(system) : nullptr;
  if (!iterator) {
    return;
  }
  slot<Call>(iterator, kIteratorAddRef)(iterator);
  slot<Call>(iterator, kIteratorMoveFirst)(iterator);
  while (void* entity = slot<Get>(iterator, kIteratorNext)(iterator)) {
    visit(entity);
  }
  slot<Call>(iterator, kIteratorRelease)(iterator);
}

void each_of_class(const char* class_name, const std::function<void(void* entity)>& visit) {
  using Call = void(__fastcall*)(void*);
  void* system = entities();
  if (!system || !class_name) {
    return;
  }
  void* registry = at_offset<Get>(system, kClassRegistry)(system);
  void* wanted =
      registry ? at_offset<FindClass>(registry, kFindClass)(registry, class_name) : nullptr;
  void* iterator = wanted ? slot<Get>(system, kIteratorSlot)(system) : nullptr;
  if (!iterator) {
    return;
  }
  slot<Call>(iterator, kIteratorAddRef)(iterator);
  slot<Call>(iterator, kIteratorMoveFirst)(iterator);
  while (void* entity = slot<Get>(iterator, kIteratorNext)(iterator)) {
    if (slot<Get>(entity, kEntityClass)(entity) == wanted) {
      visit(entity);
    }
  }
  slot<Call>(iterator, kIteratorRelease)(iterator);
}

bool load_character(void* entity, int character_slot, const char* model) {
  return entity && model &&
         slot<LoadCharacter>(entity, kLoadCharacter)(entity, character_slot, model, 0) >= 0;
}

void rotate(void* entity, const float rotation[4]) {
  slot<SetRotation>(entity, kSetRotation)(entity, rotation, 0);
}

void place(void* entity, const float position[3], const float angles[3]) {
  float rotation[4];
  rotation_xyz(angles, rotation);
  slot<SetPos>(entity, kSetPos)(entity, position, 0, false, false);
  slot<SetRotation>(entity, kSetRotation)(entity, rotation, 0);
}

void place_rotated(void* entity, const float position[3], const float rotation[4]) {
  slot<SetPos>(entity, kSetPos)(entity, position, 0, false, false);
  slot<SetRotation>(entity, kSetRotation)(entity, rotation, 0);
}

void place_in_world(void* entity, const float position[3], const float rotation[4]) {
  using SetWorldTM = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);
  float turn[4] = {rotation[0], rotation[1], rotation[2], rotation[3]};
  pose_math::normalize(turn);
  float matrix[12];
  pose_math::to_matrix(position, turn, world_scale(entity), matrix);
  slot<SetWorldTM>(entity, kSetWorldTM)(entity, matrix, 0);
}

void redraw(void* entity) {
  if (!entity) {
    return;
  }
  float position[3];
  float rotation[4];
  world_pose(entity, position, rotation);
  const float lifted[3] = {position[0], position[1], position[2] + 0.0001f};
  place_in_world(entity, lifted, rotation);
  place_in_world(entity, position, rotation);
}

bool play(void* entity, const Clip& clip) {
  void* animations = nullptr;
  void* skeleton = entity && clip.name ? skeleton_of(entity, &animations) : nullptr;
  if (!skeleton || !animations) {
    return false;
  }
  const int id = slot<AnimIdByName>(animations, kAnimIdByName)(animations, clip.name);
  if (id < 0) {
    return false;
  }
  AnimationParams params{};
  params.transition = clip.blend;
  params.key_time = -1.0f;
  params.speed = 1.0f;
  params.weight = 1.0f;
  params.layer = kLayer;
  params.layer_weight = 1.0f;
  params.flags = clip.loop ? kLoopAnimation : kRepeatLastKey;
  const bool started =
      slot<StartAnimationById>(skeleton, kStartAnimationById)(skeleton, id, &params);
  slot<LayerPlaybackSpeed>(skeleton, kLayerPlaybackSpeed)(skeleton, kLayer, clip.speed);
  return started;
}

void set_time(void* entity, float time) {
  void* skeleton = entity ? skeleton_of(entity, nullptr) : nullptr;
  if (skeleton) {
    const float clamped = time < 0.0f ? 0.0f : (time > 1.0f ? 1.0f : time);
    slot<SetLayerNormalizedTime>(skeleton, kSetLayerNormalizedTime)(skeleton, kLayer, clamped);
  }
}

void stop(void* entity) {
  void* skeleton = entity ? skeleton_of(entity, nullptr) : nullptr;
  if (skeleton) {
    slot<StopAnimationInLayer>(skeleton, kStopAnimationInLayer)(skeleton, kLayer, 0.0f);
  }
}

void hide(void* entity, bool hidden) {
  if (entity) {
    slot<Hide>(entity, kHide)(entity, hidden);
  }
}

bool is_hidden(void* entity) {
  using IsHidden = bool(__fastcall*)(void* entity);
  return entity && slot<IsHidden>(entity, kIsHidden)(entity);
}

}  // namespace kcds::runtime::props::entity
