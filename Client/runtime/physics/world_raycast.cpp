#include "world_raycast.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "line_of_sight.hpp"
#include "local_world.hpp"

namespace kcds::runtime::physics {

namespace {

namespace integration = kcd2s::integration;

// CEntity: its id, and GetPhysics.
constexpr std::size_t kEntityId = 12;
constexpr std::size_t kGetPhysics = 79;
// I3DEngine GetMaterialManager, IMaterialManager GetSurfaceType(id, why), ISurfaceType GetName
// (vtable slots, as calls them).
constexpr std::size_t kGetMaterialManager = 197;
constexpr std::size_t kGetSurfaceType = 20;
constexpr std::size_t kSurfaceName = 3;
// How far past a hit the next trace of every hit starts (metres).
constexpr float kStepPast = 0.02f;

RaycastNatives natives;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

int object_types(integration::RayMode mode) {
  switch (mode) {
    case integration::RayMode::Anything:
      return kHitAnything;
    case integration::RayMode::Ground:
      return kHitGround;
    case integration::RayMode::Terrain:
      return kHitTerrainOnly;
    case integration::RayMode::Cover:
      break;
  }
  return kHitCover;
}

void* own_physics() {
  void* entity = natives.local_entity ? natives.local_entity() : nullptr;
  if (!entity) {
    return nullptr;
  }
  using GetPhysics = void*(__fastcall*)(void* entity);
  return slot<GetPhysics>(entity, kGetPhysics)(entity);
}

std::uint32_t network_id_of_collider(void* collider) {
  void* entity = entity_of(collider);
  if (!entity || !natives.network_id_of) {
    return 0;
  }
  std::uint32_t engine_id = 0;
  std::memcpy(&engine_id, static_cast<std::uint8_t*>(entity) + kEntityId, sizeof(engine_id));
  return engine_id ? natives.network_id_of(engine_id) : 0;
}

// The surface type's name, cut to what travels; empty when the engine has none.
std::string surface_name(int surface) {
  void* engine = natives.engine_3d ? *natives.engine_3d : nullptr;
  if (!engine) {
    return {};
  }
  using GetManager = void*(__fastcall*)(void* engine);
  using GetSurfaceType = void*(__fastcall*)(void* manager, int id, const char* why);
  using GetName = const char*(__fastcall*)(void* surface_type);
  void* manager = slot<GetManager>(engine, kGetMaterialManager)(engine);
  void* type =
      manager ? slot<GetSurfaceType>(manager, kGetSurfaceType)(manager, surface, nullptr) : nullptr;
  const char* name = type ? slot<GetName>(type, kSurfaceName)(type) : nullptr;
  if (!name) {
    return {};
  }
  std::string result(name);
  if (result.size() > integration::kMaxSurfaceName) {
    result.resize(integration::kMaxSurfaceName);
  }
  return result;
}

// A text cut to what a hit carries.
std::string fitted(const std::string& text) {
  return text.size() > integration::kMaxRayText ? text.substr(0, integration::kMaxRayText) : text;
}

integration::RayHitKind kind_of(const std::string& kind) {
  if (kind == "terrain") {
    return integration::RayHitKind::Terrain;
  }
  if (kind == "entity") {
    return integration::RayHitKind::Entity;
  }
  if (kind == "vegetation") {
    return integration::RayHitKind::Vegetation;
  }
  if (kind == "brush") {
    return integration::RayHitKind::Brush;
  }
  return integration::RayHitKind::Static;
}

// What a hit or a level entity is and the level object it names.
void describe_into(const HitDescription& described, integration::WorldRayHit& result) {
  result.kind = kind_of(described.kind);
  result.model = fitted(described.model);
  result.guid = fitted(described.guid);
  result.name = fitted(described.name);
  result.entity_class = fitted(described.entity_class);
  if (described.object != 0 && std::isfinite(described.object_at[0])) {
    result.object =
        described.object == 1 ? integration::RayObject::Brush : integration::RayObject::Entity;
    std::memcpy(result.object_at, described.object_at, sizeof(result.object_at));
    std::memcpy(result.object_rotation, described.object_rotation, sizeof(result.object_rotation));
    std::memcpy(result.object_scale, described.object_scale, sizeof(result.object_scale));
  }
}

integration::WorldRayHit hit_of(const RayHit& hit, float distance) {
  integration::WorldRayHit result;
  std::memcpy(result.position, hit.position, sizeof(result.position));
  std::memcpy(result.normal, hit.normal, sizeof(result.normal));
  result.distance = distance;
  result.terrain = hit.terrain;
  result.entity = hit.terrain ? 0 : network_id_of_collider(hit.collider);
  result.surface = surface_name(hit.surface);
  // As the client's own rays read it.
  describe_into(describe_hit(hit), result);
  return result;
}

// The level's entities around the point, as hits with their own position and
// their distance from the centre, nearest first; the normal points up.
std::vector<integration::WorldRayHit> entities_around(const integration::WorldRayRequest& ray) {
  std::vector<integration::WorldRayHit> hits;
  for (const NearEntity& near : level_entities_near(ray.center, ray.radius, ray.max_hits)) {
    integration::WorldRayHit hit;
    std::memcpy(hit.position, near.position, sizeof(hit.position));
    hit.normal[2] = 1.0f;
    hit.distance = near.distance;
    describe_into(near.described, hit);
    // One that cannot travel (a pose out of the world) would void the whole answer.
    if (integration::world_ray_detail::valid_hit(hit)) {
      hits.push_back(std::move(hit));
    }
  }
  return hits;
}

// The ray's hits, nearest first, up to its max_hits; Unavailable when the game cannot trace.
TraceResult trace_hits(const integration::WorldRayRequest& ray,
                       std::vector<integration::WorldRayHit>& hits) {
  const float direction[3] = {ray.to[0] - ray.from[0], ray.to[1] - ray.from[1],
                              ray.to[2] - ray.from[2]};
  const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                 direction[2] * direction[2]);
  std::vector<void*> skipped;
  if (ray.mode == integration::RayMode::Anything) {
    if (void* own = own_physics()) {
      skipped.push_back(own);
    }
  }
  float travelled = 0;
  while (hits.size() < ray.max_hits && travelled < length) {
    float start[3];
    for (int i = 0; i < 3; ++i) {
      start[i] = ray.from[i] + direction[i] * (travelled / length);
    }
    RayHit hit;
    const TraceResult result =
        trace(start, ray.to, object_types(ray.mode), skipped.data(), int(skipped.size()), hit);
    if (result == TraceResult::Unavailable) {
      return hits.empty() ? TraceResult::Unavailable : TraceResult::Hit;
    }
    if (result == TraceResult::Miss) {
      break;
    }
    hits.push_back(hit_of(hit, travelled + hit.distance));
    travelled += hit.distance + kStepPast;
    if (!hit.terrain && hit.collider) {
      skipped.push_back(hit.collider);
    }
  }
  return hits.empty() ? TraceResult::Miss : TraceResult::Hit;
}

// The replicated bodies within the radius, nearest first, at most kMaxBodiesInRadius.
std::vector<integration::WorldBody> bodies_around(const integration::WorldRayRequest& ray) {
  std::vector<std::pair<float, integration::WorldBody>> near;
  if (natives.replicated_bodies) {
    for (const integration::WorldBody& body : natives.replicated_bodies()) {
      const float dx = body.position[0] - ray.center[0];
      const float dy = body.position[1] - ray.center[1];
      const float dz = body.position[2] - ray.center[2];
      const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (distance <= ray.radius) {
        near.emplace_back(distance, body);
      }
    }
  }
  std::sort(near.begin(), near.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<integration::WorldBody> result;
  for (const auto& [distance, body] : near) {
    if (result.size() >= integration::kMaxBodiesInRadius) {
      break;
    }
    result.push_back(body);
  }
  return result;
}

integration::WorldRayReply answer(const integration::WorldRayRequest& ray) {
  integration::WorldRayReply reply;
  reply.request = ray.request;
  reply.query = ray.query;
  if (ray.query == integration::RayQuery::Entities) {
    reply.hits = entities_around(ray);
    reply.status = reply.hits.empty() ? integration::RayStatus::Miss : integration::RayStatus::Hit;
    return reply;
  }
  if (ray.query == integration::RayQuery::Bodies) {
    reply.bodies = bodies_around(ray);
    reply.status =
        reply.bodies.empty() ? integration::RayStatus::Miss : integration::RayStatus::Hit;
    return reply;
  }
  const TraceResult result = trace_hits(ray, reply.hits);
  if (result == TraceResult::Unavailable) {
    reply.status = integration::RayStatus::Unavailable;
  } else if (result == TraceResult::Miss) {
    reply.status = integration::RayStatus::Miss;
  } else {
    reply.status = integration::RayStatus::Hit;
  }
  return reply;
}

}  // namespace

void install_raycasts(const RaycastNatives& given) {
  natives = given;
}

integration::WorldRayHit server_hit(const RayHit& hit, float distance) {
  return hit_of(hit, distance);
}

std::string surface_type_name(int surface) {
  return surface_name(surface);
}

void answer_rays(const std::vector<integration::WorldRayRequest>& rays) {
  for (const integration::WorldRayRequest& ray : rays) {
    const integration::WorldRayReply reply = answer(ray);
    if (natives.send_reply) {
      natives.send_reply(reply);
    }
  }
}

}  // namespace kcds::runtime::physics
