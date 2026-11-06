// Whether the world's solid geometry lies between two points, with the game's own ray test
// (WHGame 1.5.5): gEnv->pPhysicalWorld's RayWorldIntersection (vtable + 280: the ray's parameters,
// a name tag, the calling thread), as CryAction's own ray asks it. The parameters
// (112 bytes): the ray's origin at + 24 and its length and direction at + 36, the kinds of things
// it hits at + 48 and how at + 52 (the game's own 0x107 and 0x1000F: terrain, static objects, and
// resting and moving rigid bodies; living bodies are not hit), and one hit (80 bytes) at + 56, with
// the count at + 64.
//
// Engine thread.
#ifndef KCDS_RUNTIME_PHYSICS_LINE_OF_SIGHT_HPP
#define KCDS_RUNTIME_PHYSICS_LINE_OF_SIGHT_HPP

namespace kcds::runtime::physics {

// physical_world: the global holding gEnv->pPhysicalWorld. False when it is missing.
bool install(void* const* physical_world);

// True when nothing solid lies between the two points (or the test cannot be made).
bool clear_between(const float from[3], const float to[3]);

// True when something solid lies between the two points; `distance` is then how far from `from`
// the first thing is (the hit's distance, its first field). False when nothing is (or the test
// cannot be made).
bool first_hit(const float from[3], const float to[3], float& distance);

// The same, with the hit surface's normal (ray_hit + 48, after the point at + 36); the normal is
// straight up when the game gave none of unit length.
bool first_hit(const float from[3], const float to[3], float& distance, float normal[3]);

// The kinds of things a ray hits (the params' + 48): the game's own line-of-sight set above, the
// physics script binding's default (0x11F, living bodies and independent entities
// too) and the ground (terrain and static geometry).
inline constexpr int kHitCover = 0x107;
inline constexpr int kHitAnything = 0x11F;
inline constexpr int kHitGround = 0x101;
inline constexpr int kHitTerrainOnly = 0x100;

// One ray_hit as Physics.RayWorldIntersection reads it into a script table: dist at
// + 0, pCollider + 8, surface_idx + 24 (a short, the binding's "surface"), pt + 36, n + 48,
// bTerrain + 60.
struct RayHit {
  float distance = 0;
  float position[3] = {0, 0, 0};
  float normal[3] = {0, 0, 0};
  bool terrain = false;
  int surface = 0;           // the surface type's id
  void* collider = nullptr;  // IPhysicalEntity
};

enum class TraceResult { Unavailable, Miss, Hit };

// Traces the segment against `object_types`, skipping the `skip_count` physical entities of
// `skip` (or nothing), and reports the first solid thing it met.
TraceResult trace(const float from[3], const float to[3], int object_types, void* const* skip,
                  int skip_count, RayHit& hit);

// The engine entity (IEntity) that owns a physical entity: its foreign data of kind 2
// (PHYS_FOREIGN_ID_ENTITY, physical entity slot 10, as asks it). Null for terrain,
// static geometry and anything else.
void* entity_of(void* collider);

}  // namespace kcds::runtime::physics

#endif  // KCDS_RUNTIME_PHYSICS_LINE_OF_SIGHT_HPP
