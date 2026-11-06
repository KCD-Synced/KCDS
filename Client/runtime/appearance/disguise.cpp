#include "disguise.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <unordered_map>

namespace kcds::runtime::appearance::disguise {
namespace {

using LoadGeometry = int(__fastcall*)(void* entity, int slot, const char* file,
                                      const char* geometry, int flags);
using FreeSlot = void(__fastcall*)(void* entity, int slot);
using SetSlotFlags = void(__fastcall*)(void* entity, int slot, std::uint32_t flags);
using GetSlotFlags = std::uint32_t(__fastcall*)(void* entity, int slot);
using GetPhysics = void*(__fastcall*)(void* entity);
using GetLocalBounds = void(__fastcall*)(void* render, float* bounds);
using PhysicsParams = int(__fastcall*)(void* physics, void* params, int thread_safe);

constexpr std::size_t kLoadGeometrySlot = 108;
constexpr std::size_t kFreeSlotSlot = 87;
constexpr std::size_t kSetSlotFlagsSlot = 98;
constexpr std::size_t kGetSlotFlagsSlot = 99;
constexpr std::size_t kGetPhysicsSlot = 79;
// The render component's GetLocalBounds: the drawn slots' box (calls it).
constexpr std::size_t kGetLocalBoundsSlot = 18;
constexpr unsigned kRenderComponent = 0;
// The living entity's SetParams / GetParams (CLivingEntity).
constexpr std::size_t kSetParamsSlot = 4;
constexpr std::size_t kGetParamsSlot = 5;
// The character of a player's entity.
constexpr int kCharacterSlot = 0;
// LoadGeometry's slot for "the next free one".
constexpr int kFreeSlot = -1;
// The slot flags that draw a slot.
constexpr std::uint32_t kDrawBits = 3;

// pe_player_dimensions (type 1) as CLivingEntity reads and writes it
// (SetParams); a field the engine must leave alone carries the unused bit pattern. The
// unprojection is not optional: SetParams moves the body by dir_unproj * max_unproj, so they
// keep the engine's defaults (0, 0, 1) and 0.
constexpr int kPlayerDimensions = 1;
constexpr std::uint32_t kUnusedFloat = 0xFFBFFFFFu;
constexpr int kUnusedInt = static_cast<int>(0x80000000u);
struct Dimensions {
  int type = kPlayerDimensions;
  float height_pivot;
  float height_eye;
  float size_collider[3];
  float height_collider;
  float head_radius;
  float height_head;
  float dir_unproj[3];
  float max_unproj;
  int use_capsule;
  float ground_contact_eps;
};
// The collider of a mesh stays between these (a cup is not a needle, a cart not a house).
constexpr float kMinRadius = 0.15f;
constexpr float kMaxRadius = 2.5f;
constexpr float kMinHalfHeight = 0.05f;
constexpr float kMaxHalfHeight = 2.5f;

// A body, the mesh asked for and the mesh it wears in its mesh slot.
struct Body {
  void* entity = nullptr;
  std::string requested;
  std::string worn;
  int slot = -1;
  // The character slot's draw bits before the mesh was put on.
  std::uint32_t drawn = 0;
  // The collider sized around the mesh and the game's own one it replaces.
  bool resized = false;
  Dimensions mesh_collider{};
  Dimensions own_collider{};
};

Logger log_line = nullptr;
ComponentLookup component = nullptr;
Body local_body;
std::unordered_map<std::uint32_t, Body> replicas;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

std::uint32_t slot_flags(void* entity, int index) {
  return slot<GetSlotFlags>(entity, kGetSlotFlagsSlot)(entity, index);
}

void set_slot_flags(void* entity, int index, std::uint32_t flags) {
  slot<SetSlotFlags>(entity, kSetSlotFlagsSlot)(entity, index, flags);
}

void* physics_of(void* entity) {
  return slot<GetPhysics>(entity, kGetPhysicsSlot)(entity);
}

// Dimensions with every field the engine must leave alone, the unprojection at its defaults.
Dimensions unused_dimensions() {
  Dimensions dimensions;
  for (float* field :
       {&dimensions.height_pivot, &dimensions.height_eye, &dimensions.size_collider[0],
        &dimensions.size_collider[1], &dimensions.size_collider[2], &dimensions.height_collider,
        &dimensions.head_radius, &dimensions.height_head, &dimensions.ground_contact_eps}) {
    std::memcpy(field, &kUnusedFloat, sizeof(float));
  }
  dimensions.dir_unproj[0] = 0.0f;
  dimensions.dir_unproj[1] = 0.0f;
  dimensions.dir_unproj[2] = 1.0f;
  dimensions.max_unproj = 0.0f;
  dimensions.use_capsule = kUnusedInt;
  return dimensions;
}

bool read_collider(void* physics, Dimensions& dimensions) {
  dimensions = unused_dimensions();
  return slot<PhysicsParams>(physics, kGetParamsSlot)(physics, &dimensions, 0) != 0;
}

// Sets only the collider's size and height; the pivot, the eye and the head stay the game's.
void write_collider(void* physics, const Dimensions& collider) {
  Dimensions dimensions = unused_dimensions();
  std::memcpy(dimensions.size_collider, collider.size_collider, sizeof(collider.size_collider));
  dimensions.height_collider = collider.height_collider;
  slot<PhysicsParams>(physics, kSetParamsSlot)(physics, &dimensions, 0);
}

bool same_size(const Dimensions& a, const Dimensions& b) {
  const float tolerance = 1e-3f;
  for (int axis = 0; axis < 3; ++axis) {
    if (std::fabs(a.size_collider[axis] - b.size_collider[axis]) > tolerance) {
      return false;
    }
  }
  return std::fabs(a.height_collider - b.height_collider) <= tolerance;
}

// The game's collider of the body resized around what its entity draws now: the mesh, the
// character slot no longer drawn.
bool collider_around_mesh(void* entity, const Dimensions& own, Dimensions& sized) {
  void* render = component ? component(entity, kRenderComponent) : nullptr;
  if (!render) {
    return false;
  }
  float bounds[6] = {1, 1, 1, 0, 0, 0};
  slot<GetLocalBounds>(render, kGetLocalBoundsSlot)(render, bounds);
  if (bounds[0] > bounds[3] || bounds[2] > bounds[5]) {
    return false;
  }
  const float width = std::max(bounds[3] - bounds[0], bounds[4] - bounds[1]);
  const float radius = std::clamp(width * 0.5f, kMinRadius, kMaxRadius);
  // A capsule reaches its radius above and below its cylinder.
  const float caps = own.use_capsule ? radius : 0.0f;
  // The game keeps a gap under the collider for the legs to step with; the mesh's collider keeps
  // the same gap and reaches from it to the mesh's top, at least as high as its own shape is.
  const float own_caps = own.use_capsule ? own.size_collider[0] : 0.0f;
  const float gap = std::max(own.height_collider - own.size_collider[2] - own_caps, 0.0f);
  const float span = std::max(bounds[5] - gap, 2.0f * (kMinHalfHeight + caps));
  const float half = std::clamp(span * 0.5f - caps, kMinHalfHeight, kMaxHalfHeight);
  sized = own;
  sized.size_collider[0] = radius;
  sized.size_collider[1] = radius;
  sized.size_collider[2] = half;
  sized.height_collider = gap + caps + half;
  return true;
}

void resize_collider(Body& body) {
  void* physics = physics_of(body.entity);
  Dimensions own;
  if (!physics || !read_collider(physics, own) ||
      !collider_around_mesh(body.entity, own, body.mesh_collider)) {
    return;
  }
  body.own_collider = own;
  body.resized = true;
  write_collider(physics, body.mesh_collider);
  if (log_line) {
    log_line("APPEARANCE disguise collider radius=%.2f half=%.2f center=%.2f (own %.2f %.2f %.2f)",
             body.mesh_collider.size_collider[0], body.mesh_collider.size_collider[2],
             body.mesh_collider.height_collider, own.size_collider[0], own.size_collider[2],
             own.height_collider);
  }
}

// The game sets the collider again when the body changes stance; a disguised body keeps the
// mesh's, and remembers the game's newest one to give back.
void keep_collider(Body& body) {
  void* physics = physics_of(body.entity);
  Dimensions current;
  if (!physics || !read_collider(physics, current) || same_size(current, body.mesh_collider)) {
    return;
  }
  body.own_collider = current;
  write_collider(physics, body.mesh_collider);
  if (log_line) {
    log_line("APPEARANCE disguise collider set again: found %.3f %.3f %.3f center %.3f",
             current.size_collider[0], current.size_collider[1], current.size_collider[2],
             current.height_collider);
  }
}

void restore_collider(Body& body) {
  if (!body.resized) {
    return;
  }
  body.resized = false;
  if (void* physics = physics_of(body.entity)) {
    write_collider(physics, body.own_collider);
  }
}

void take_off(Body& body) {
  if (body.slot < 0) {
    return;
  }
  restore_collider(body);
  slot<FreeSlot>(body.entity, kFreeSlotSlot)(body.entity, body.slot);
  const std::uint32_t flags = slot_flags(body.entity, kCharacterSlot);
  set_slot_flags(body.entity, kCharacterSlot, flags | body.drawn);
  body.slot = -1;
  body.worn.clear();
}

void put_on(Body& body, const std::string& model) {
  const int mesh = slot<LoadGeometry>(body.entity, kLoadGeometrySlot)(body.entity, kFreeSlot,
                                                                      model.c_str(), nullptr, 0);
  if (mesh < 0) {
    if (log_line) {
      log_line("APPEARANCE disguise %s not loaded", model.c_str());
    }
    return;
  }
  const std::uint32_t flags = slot_flags(body.entity, kCharacterSlot);
  body.drawn = flags & kDrawBits;
  set_slot_flags(body.entity, kCharacterSlot, flags & ~kDrawBits);
  body.slot = mesh;
  body.worn = model;
  if (log_line) {
    log_line("APPEARANCE disguise %s in slot %d", model.c_str(), mesh);
  }
  resize_collider(body);
}

std::string keep(Body& body, void* entity, const std::string& model) {
  if (body.entity != entity) {
    // A new body: the old one's mesh slot and collider went with it.
    body = Body{};
    body.entity = entity;
  }
  if (!entity) {
    return {};
  }
  if (body.requested != model) {
    take_off(body);
    body.requested = model;
    if (!model.empty()) {
      put_on(body, model);
    }
  }
  if (body.slot >= 0) {
    // The game may draw the character slot again (a respawn of its character); a disguised
    // body keeps it hidden.
    const std::uint32_t flags = slot_flags(entity, kCharacterSlot);
    if (flags & kDrawBits) {
      set_slot_flags(entity, kCharacterSlot, flags & ~kDrawBits);
    }
    if (body.resized) {
      keep_collider(body);
    }
  }
  return body.worn;
}

}  // namespace

void install(ComponentLookup lookup, Logger logger) {
  component = lookup;
  log_line = logger;
}

std::string keep_local(void* entity, const std::string& model) {
  return keep(local_body, entity, model);
}

void keep_replica(std::uint32_t player, void* entity, const std::string& model) {
  keep(replicas[player], entity, model);
}

void forget(std::uint32_t player) {
  replicas.erase(player);
}

}  // namespace kcds::runtime::appearance::disguise
