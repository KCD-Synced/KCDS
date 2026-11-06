#include "line_of_sight.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::physics {

namespace {

constexpr std::size_t kRayWorldIntersection = 280 / 8;
constexpr std::size_t kParamsSize = 112;
constexpr std::size_t kParamsOrigin = 24;
constexpr std::size_t kParamsDirection = 36;
constexpr std::size_t kParamsObjectTypes = 48;
constexpr std::size_t kParamsFlags = 52;
constexpr std::size_t kParamsHits = 56;
constexpr std::size_t kParamsMaxHits = 64;
// The entities to skip: their count at + 80 and a pointer to their array at + 88 (as
// fills them from the script binding's ignore list).
constexpr std::size_t kParamsSkipCount = 80;
constexpr std::size_t kParamsSkipList = 88;
constexpr std::size_t kHitSize = 128;
// ray_hit (80 bytes) as reads it.
constexpr std::size_t kHitDistance = 0;
constexpr std::size_t kHitCollider = 8;
constexpr std::size_t kHitSurface = 24;  // short, what the binding gives as "surface"
constexpr std::size_t kHitPoint = 36;
constexpr std::size_t kHitNormal = 48;
constexpr std::size_t kHitTerrain = 60;
// IPhysicalEntity::GetForeignData (slot 10) and PHYS_FOREIGN_ID_ENTITY.
constexpr std::size_t kGetForeignData = 80;
constexpr int kForeignEntity = 2;
// The game's own: terrain, static objects, resting and moving rigid bodies.
constexpr int kObjectTypes = 0x107;
constexpr unsigned kFlags = 0x1000F;
// The calling thread as the game's main-thread rays give it.
constexpr int kCaller = 4;
constexpr char kNameTag[] = "RayWorldIntersection(KCDS)";

using RayWorldIntersection = int(__fastcall*)(void* world, void* params, const char* tag,
                                              int caller);

void* const* world_slot = nullptr;

}  // namespace

bool install(void* const* physical_world) {
  world_slot = physical_world;
  return world_slot != nullptr;
}

bool clear_between(const float from[3], const float to[3]) {
  float distance = 0;
  return !first_hit(from, to, distance);
}

bool first_hit(const float from[3], const float to[3], float& distance) {
  float normal[3];
  return first_hit(from, to, distance, normal);
}

bool first_hit(const float from[3], const float to[3], float& distance, float normal[3]) {
  normal[0] = 0.0f;
  normal[1] = 0.0f;
  normal[2] = 1.0f;
  RayHit hit;
  if (trace(from, to, kObjectTypes, nullptr, 0, hit) != TraceResult::Hit) {
    return false;
  }
  distance = hit.distance;
  const float length = std::sqrt(hit.normal[0] * hit.normal[0] + hit.normal[1] * hit.normal[1] +
                                 hit.normal[2] * hit.normal[2]);
  if (std::isfinite(length) && std::fabs(length - 1.0f) < 0.01f) {
    std::memcpy(normal, hit.normal, sizeof(hit.normal));
  }
  return true;
}

TraceResult trace(const float from[3], const float to[3], int object_types, void* const* skip,
                  int skip_count, RayHit& result) {
  void* world = world_slot ? *world_slot : nullptr;
  if (!world) {
    return TraceResult::Unavailable;
  }
  alignas(16) std::uint8_t params[kParamsSize] = {};
  alignas(16) std::uint8_t hit[kHitSize] = {};
  const float direction[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
  std::memcpy(params + kParamsOrigin, from, 3 * sizeof(float));
  std::memcpy(params + kParamsDirection, direction, sizeof(direction));
  std::memcpy(params + kParamsObjectTypes, &object_types, sizeof(object_types));
  std::memcpy(params + kParamsFlags, &kFlags, sizeof(kFlags));
  void* hits = hit;
  std::memcpy(params + kParamsHits, &hits, sizeof(hits));
  const int max_hits = 1;
  std::memcpy(params + kParamsMaxHits, &max_hits, sizeof(max_hits));
  // The entities to skip, as the script binding passes its own (a pointer to an array).
  if (skip && skip_count > 0) {
    const void* list = skip;
    std::memcpy(params + kParamsSkipCount, &skip_count, sizeof(skip_count));
    std::memcpy(params + kParamsSkipList, &list, sizeof(list));
  }
  auto test = (*static_cast<RayWorldIntersection**>(world))[kRayWorldIntersection];
  if (test(world, params, kNameTag, kCaller) <= 0) {
    return TraceResult::Miss;
  }
  int terrain = 0;
  std::int16_t surface = 0;
  std::memcpy(&surface, hit + kHitSurface, sizeof(surface));
  result.surface = surface;
  std::memcpy(&result.distance, hit + kHitDistance, sizeof(result.distance));
  std::memcpy(&result.collider, hit + kHitCollider, sizeof(result.collider));
  std::memcpy(result.position, hit + kHitPoint, sizeof(result.position));
  std::memcpy(result.normal, hit + kHitNormal, sizeof(result.normal));
  std::memcpy(&terrain, hit + kHitTerrain, sizeof(terrain));
  result.terrain = terrain != 0;
  return TraceResult::Hit;
}

void* entity_of(void* collider) {
  if (!collider) {
    return nullptr;
  }
  using GetForeignData = void*(__fastcall*)(void* physical, int kind);
  auto get = reinterpret_cast<GetForeignData>(
      (*static_cast<void***>(collider))[kGetForeignData / sizeof(void*)]);
  return get(collider, kForeignEntity);
}

}  // namespace kcds::runtime::physics
