// The client resources' own questions about this game's world: Camera.getPose and
// rayFromScreen, World.raycast, traceAll, groundHeight, snapToGround and entitiesAround in client
// scripts (Client/resources/world.cjs). They answer inside the call: the runtime sends
// { op: 'syncRequest', id, request } and waits for { op: 'syncReply', id, value }, which this
// module gives on the next engine frame.
//
//   { op: 'localWorldCamera' }
//     -> { m: [12 numbers], fov } the view camera (3x4 rows: right, forward and up are its
//        columns 0, 1 and 2, the eye column 3; fov the vertical field of view in radians), or null
//        outside a world.
//   { op: 'localWorldRay', from, to, mode, max, ignoreSelf }
//     mode 0 = cover, 1 = anything, 2 = ground (line_of_sight.hpp's sets); max 1 .. 8 hits, each
//     found by tracing again a hand's breadth past the one before.
//     -> { status: 'hit' | 'miss' | 'unavailable', hits: [hit] }, a hit being
//        { p: [x, y, z], n: [x, y, z], d, terrain, surface, kind, model, entity, cls, name, guid }:
//        kind 'terrain' | 'entity' | 'brush' | 'vegetation' | 'static'; model the mesh path of a
//        brush or a vegetation instance (else ''); entity the engine id of the entity hit (0 for
//        none), cls its class, name its name and guid its EntityGuid as sixteen hex digits ('' for
//        one the level does not name).
//   { op: 'localWorldEntities', center, radius, cls, max, physicalOnly }
//     -> [{ entity, p: [x, y, z], d, cls, name, guid, physical }], nearest first.
//
// What a hit is: the collider's foreign data (physical entity slots 10 and 11) names an entity
// (type 2, line_of_sight.hpp entity_of) or a static render node (type 1, as level/level_edits.hpp
// finds brushes): a CBrush or a CVegetation by its RTTI name, its mesh by IRenderNode GetName (slot
// 9). An entity's class is IEntity GetClass (slot 3) with IEntityClass GetName (slot 2), its name
// at + 224, its EntityGuid the entity system's key (slot 2, level/level_edits.hpp).
//
// The entities in a sphere: with physicalOnly, the physical world's GetEntitiesInBox (slot 34)
// over the sphere's box and each one's entity; otherwise one walk of the entity system (its
// iterator, slot 22), every entity's place compared.
//
// Requests: any thread (handle_runtime_message). Answers: engine thread (frame). Replies: network
// thread (pump).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"
#include "line_of_sight.hpp"

namespace kcds::runtime::physics {

// What a ray's hit is, for the client's own rays and the server's (world_raycast.cpp; issue):
// kind 'terrain', 'entity', 'brush', 'vegetation' or 'static'; a brush's or vegetation
// instance's mesh; the entity's engine id, class, name and level GUID (in the level files'
// order); and the level object a level edit finds it by (object 1 a brush by its mesh and where
// the level put it, 2 an entity by its GUID, standing where it stands now; 0 none), with its turn
// (x, y, z, w) and size per axis. Engine thread.
struct HitDescription {
  std::string kind = "static";
  std::string model;
  std::uint32_t entity_id = 0;
  std::string entity_class;
  std::string name;
  std::string guid;
  int object = 0;
  float object_at[3] = {0, 0, 0};
  float object_rotation[4] = {0, 0, 0, 1};
  float object_scale[3] = {1, 1, 1};
  // A brush its layer hides: an invisible collision of the level's.
  bool unseen = false;
  // The thing's box in the world (min x, y, z, max x, y, z), when it has one: an entity's world
  // bounds, a brush's or vegetation instance's render node box (IRenderNode GetBBox, slot 5).
  bool has_box = false;
  float box[6] = {0, 0, 0, 0, 0, 0};
};

HitDescription describe_hit(const RayHit& hit);

// Whether a hit is on a level's building or rock: not the ground, an entity or a vegetation
// instance. Cheap (no brush lookup), for many rays a frame. Engine thread.
bool hit_is_building(const RayHit& hit);

// An entity the level names, near a point (World.levelEntitiesAround): what it
// is as a hit on it would say (kind 'entity', object 2), where it stands, and how far from the
// point.
struct NearEntity {
  HitDescription described;
  float position[3] = {0, 0, 0};
  float distance = 0;
};

// The entities the level names within `radius` of `center`, nearest first, at most `most`. The
// engine's iterator walks every entity once. Engine thread.
std::vector<NearEntity> level_entities_near(const float center[3], float radius, std::size_t most);

struct LocalWorldNatives {
  // The local player's engine entity, or null.
  void* (*local_entity)() = nullptr;
  // IEntitySystem, or null.
  void* (*entity_system)() = nullptr;
  // The global holding gEnv->pPhysicalWorld.
  void* const* physical_world = nullptr;
  // The view camera this frame (3x4 rows and the vertical field of view in radians); false
  // outside a world.
  bool (*view_camera)(float matrix[12], float& fov) = nullptr;
};

void install_local_world(const LocalWorldNatives& natives);

// The entities with physics whose colliders reach into the sphere's box (the physical world's
// GetEntitiesInBox), each once. Engine thread. A whip's lash finds the bodies in its reach so.
std::vector<void*> physical_entities_near(const float center[3], float radius);

// A resource runtime message; false when it is not one of these requests. Any thread.
bool handle_local_world_message(const kcd2s::sdk::Value& message);

// Answers the requests that came since the last frame (engine thread). Outside a world a ray or
// sphere answers 'unavailable' or nothing, and the camera null.
void answer_local_world(bool in_world);

// A hit as the client scripts read it (the hits of localWorldRay above), `distance` from where the
// asker measures. Engine thread.
kcd2s::sdk::Value script_hit(const RayHit& hit, float distance);

// The view camera now (3x4 rows: right, forward and up are columns 0, 1 and 2, the eye column 3;
// the vertical field of view in radians); false outside a world. Engine thread.
bool view_camera(float matrix[12], float& fov);

// The first thing the segment meets in a localWorldRay mode (1 'anything', 2 'ground', 3
// 'terrain'), the local player's body skipped, as a hit of localWorldRay; null when it meets
// nothing. Engine thread.
kcd2s::sdk::Value first_hit(const float from[3], const float to[3], int mode = 1);

// Sends the answers to the runtime (network thread).
void pump_local_world(const std::function<bool(const kcd2s::sdk::Value&)>& send);

}  // namespace kcds::runtime::physics
