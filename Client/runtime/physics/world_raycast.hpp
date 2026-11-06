// Questions about the world the server asks this game (World.raycast, traceAll, snapToGround
// and entitiesAround): the game's own RayWorldIntersection
// (line_of_sight.hpp) with the kinds of things the mode names, and the answer the server waits
// for. In the `anything` mode the local player's own body is skipped (its GetPhysics, entity slot
// 79), as the physics script binding skips the entities it is given. The hit's collider names its
// engine entity (line_of_sight.hpp entity_of), whose id (+ 12) is turned into the network id of
// the replicated body it belongs to, if any.
//
// Every hit along a ray (traceAll) is a trace after trace: each starts a hand's breadth past the
// last hit, and skips the bodies already met (terrain is never skipped, only stepped past), so the
// hits come nearest first and no body twice.
//
// A hit's surface is named as the script's Physics.GetSurfaceTypeNameById does: the
// 3D engine's material manager (slot 197), its GetSurfaceType(id, why) (slot 20, CMatMan
// over CSurfaceTypeManager, which gives mat_default for an unknown id), the
// surface type's GetName (slot 3).
//
// The bodies in a radius are the replicated bodies this game shows (the local player, other
// players' replicas, server NPCs and horses), where their entities stand.
//
// Engine thread.
#ifndef KCDS_RUNTIME_PHYSICS_WORLD_RAYCAST_HPP
#define KCDS_RUNTIME_PHYSICS_WORLD_RAYCAST_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/world_ray.hpp"
#include "line_of_sight.hpp"

namespace kcds::runtime::physics {

struct RaycastNatives {
  // The local player's engine entity, or null.
  void* (*local_entity)() = nullptr;
  // The network id of the replicated body (the local player, another player's replica, a server
  // NPC or horse) that has this engine id here; 0 for none.
  std::uint32_t (*network_id_of)(std::uint32_t engine_id) = nullptr;
  // Every replicated body this game shows, with its network id and where its entity stands.
  std::vector<kcd2s::integration::WorldBody> (*replicated_bodies)() = nullptr;
  // gEnv->p3DEngine, for the surface names; null leaves them empty.
  void* const* engine_3d = nullptr;
  // Sends the answer; false when it could not go.
  bool (*send_reply)(const kcd2s::integration::WorldRayReply& reply) = nullptr;
};

void install_raycasts(const RaycastNatives& natives);

// Answers each question and sends its answer.
void answer_rays(const std::vector<kcd2s::integration::WorldRayRequest>& rays);

// A hit as the server reads it (WORLD_RAYCAST_REPLY, WEAPON_HIT), `distance` from where the
// asker measures.
kcd2s::integration::WorldRayHit server_hit(const RayHit& hit, float distance);

// The name of a surface type by its id (mat_wood, mat_water, ...), empty when the engine has none
// (the client scripts' own rays, local_world.hpp).
std::string surface_type_name(int surface);

}  // namespace kcds::runtime::physics

#endif  // KCDS_RUNTIME_PHYSICS_WORLD_RAYCAST_HPP
