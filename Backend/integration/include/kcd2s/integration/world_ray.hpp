#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Questions about the world asked of a player's game for the server.
// Without geometry on the server, one player's game is asked to trace a segment with the
// game's physics, or to name the replicated bodies around a point, and answer what it found. Both
// messages are on the control channel (0).
//
//   WORLD_RAYCAST       : server -> a ready player's client: [tag][request u32][query u8], then
//                         for query 0 (the first hit) and 1 (every hit, nearest first)
//                         [from 3 x f32][to 3 x f32][mode u8][max hits u8], for query 2 (the
//                         bodies in a radius) [centre 3 x f32][radius f32]. mode 0 = cover
//                         (terrain, static geometry and rigid bodies; living bodies are not hit),
//                         1 = anything (living bodies too, the player's own body skipped),
//                         2 = ground (terrain and static geometry only), 3 = terrain only. Query 0
//                         asks for 1 hit, query 1 for 1 .. kMaxRayHits. Query 3 (the level's
//                         entities in a radius) [centre 3 x f32][radius f32]
//                         [max u8], a radius up to kMaxEntityRadius and 1 .. kMaxLevelEntities.
//   WORLD_RAYCAST_REPLY : client -> server: [tag][request u32][query u8][status u8]. status 0 =
//                         nothing met, 1 = found, 2 = the game could not answer (no physics
//                         world). With status 1, for query 0 and 1 [count u8] hits of
//                         [position 3 x f32][normal 3 x f32][distance f32][terrain]
//                         [entity u32][surface: length u8, bytes][kind u8][model][guid][name]
//                         [class][object u8, with object 1 or 2 [at 3 x f32][rotation 4 x f32]
//                         [scale 3 x f32]], for query 2 [count u8] bodies of [entity u32]
//                         [position 3 x f32], for query 3 [count u8] hits as for query 1, each
//                         an entity the level names (kind entity, object 2), its position where
//                         it stands and its distance from the centre. entity is the network id of a
//                         replicated body (a player, an NPC or a horse), 0 for anything else;
//                         surface is the name of the hit's surface type (empty when unknown). kind
//                         is what was met (RayHitKind); model the mesh of a brush or a vegetation
//                         instance; guid (sixteen hex digits in the level files' order), name and
//                         class the entity's; each a text of length u8 and bytes. object is the
//                         level object a level edit finds the hit by: 0 none, 1 a
//                         brush (by model and at), 2 an entity (by guid), with where the level put
//                         it (an entity: where it stands), its turn (x, y, z, w) and its size per
//                         axis.
//
// Client/runtime/physics/ implements the client side.
namespace kcd2s::integration {

inline constexpr std::uint8_t kWorldRaycastTag = 85;
inline constexpr std::uint8_t kWorldRaycastReplyTag = 86;

// The longest ray a client is asked to trace (metres) and the farthest a point lies from the
// world's origin and still travels.
inline constexpr float kMaxRayLength = 4096.0f;
inline constexpr float kMaxRayCoordinate = 100000.0f;
// The most hits one ray answers with, the widest radius bodies are looked for in (metres), the
// most bodies one answer names and the longest surface name that travels.
inline constexpr std::uint8_t kMaxRayHits = 16;
inline constexpr float kMaxBodyRadius = 512.0f;
inline constexpr std::uint8_t kMaxBodiesInRadius = 64;
// The widest radius the level's entities are looked for in (metres) and the most one answer
// names (RayQuery::Entities).
inline constexpr float kMaxEntityRadius = 64.0f;
inline constexpr std::uint8_t kMaxLevelEntities = 32;
inline constexpr std::size_t kMaxSurfaceName = 63;
// The longest model path, GUID, name or class a hit carries.
inline constexpr std::size_t kMaxRayText = 255;

// What a ray met.
enum class RayHitKind : std::uint8_t {
  Static = 0,
  Terrain = 1,
  Entity = 2,
  Vegetation = 3,
  Brush = 4
};
inline constexpr std::uint8_t kRayHitKindCount = 5;

// The level object a hit names: none, a brush or an entity.
enum class RayObject : std::uint8_t { None = 0, Brush = 1, Entity = 2 };
inline constexpr std::uint8_t kRayObjectCount = 3;

enum class RayQuery : std::uint8_t { First = 0, All = 1, Bodies = 2, Entities = 3 };
inline constexpr std::uint8_t kRayQueryCount = 4;

enum class RayMode : std::uint8_t { Cover = 0, Anything = 1, Ground = 2, Terrain = 3 };
inline constexpr std::uint8_t kRayModeCount = 4;

enum class RayStatus : std::uint8_t { Miss = 0, Hit = 1, Unavailable = 2 };
inline constexpr std::uint8_t kRayStatusCount = 3;

struct WorldRayRequest {
  std::uint32_t request = 0;
  RayQuery query = RayQuery::First;
  // The ray (RayQuery::First and All).
  float from[3] = {0.0f, 0.0f, 0.0f};
  float to[3] = {0.0f, 0.0f, 0.0f};
  RayMode mode = RayMode::Cover;
  std::uint8_t max_hits = 1;
  // The circle (RayQuery::Bodies and Entities; max_hits is the most entities).
  float center[3] = {0.0f, 0.0f, 0.0f};
  float radius = 0.0f;

  friend bool operator==(const WorldRayRequest& a, const WorldRayRequest& b) {
    return a.request == b.request && a.query == b.query &&
           std::memcmp(a.from, b.from, sizeof(a.from)) == 0 &&
           std::memcmp(a.to, b.to, sizeof(a.to)) == 0 && a.mode == b.mode &&
           a.max_hits == b.max_hits && std::memcmp(a.center, b.center, sizeof(a.center)) == 0 &&
           a.radius == b.radius;
  }
};

// One thing a ray met.
struct WorldRayHit {
  float position[3] = {0.0f, 0.0f, 0.0f};
  float normal[3] = {0.0f, 0.0f, 0.0f};
  float distance = 0.0f;  // metres from the ray's start
  bool terrain = false;
  std::uint32_t entity = 0;  // network id of the replicated body hit, 0 for none
  std::string surface;       // the surface type's name, empty when unknown
  RayHitKind kind = RayHitKind::Static;
  std::string model;         // a brush's or vegetation instance's mesh, else empty
  std::string guid;          // an entity's level GUID, empty for none
  std::string name;          // an entity's name
  std::string entity_class;  // an entity's class
  RayObject object = RayObject::None;
  float object_at[3] = {0.0f, 0.0f, 0.0f};
  float object_rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // x, y, z, w
  float object_scale[3] = {1.0f, 1.0f, 1.0f};

  friend bool operator==(const WorldRayHit& a, const WorldRayHit& b) {
    return std::memcmp(a.position, b.position, sizeof(a.position)) == 0 &&
           std::memcmp(a.normal, b.normal, sizeof(a.normal)) == 0 && a.distance == b.distance &&
           a.terrain == b.terrain && a.entity == b.entity && a.surface == b.surface &&
           a.kind == b.kind && a.model == b.model && a.guid == b.guid && a.name == b.name &&
           a.entity_class == b.entity_class && a.object == b.object &&
           std::memcmp(a.object_at, b.object_at, sizeof(a.object_at)) == 0 &&
           std::memcmp(a.object_rotation, b.object_rotation, sizeof(a.object_rotation)) == 0 &&
           std::memcmp(a.object_scale, b.object_scale, sizeof(a.object_scale)) == 0;
  }
};

// One replicated body inside the radius, where the asked game shows it.
struct WorldBody {
  std::uint32_t entity = 0;
  float position[3] = {0.0f, 0.0f, 0.0f};

  friend bool operator==(const WorldBody& a, const WorldBody& b) {
    return a.entity == b.entity && std::memcmp(a.position, b.position, sizeof(a.position)) == 0;
  }
};

struct WorldRayReply {
  std::uint32_t request = 0;
  RayQuery query = RayQuery::First;
  RayStatus status = RayStatus::Miss;
  // with status Hit for RayQuery::First, All and Entities, nearest first
  std::vector<WorldRayHit> hits;
  std::vector<WorldBody> bodies;  // with status Hit for RayQuery::Bodies

  friend bool operator==(const WorldRayReply& a, const WorldRayReply& b) {
    return a.request == b.request && a.query == b.query && a.status == b.status &&
           a.hits == b.hits && a.bodies == b.bodies;
  }
};

namespace world_ray_detail {

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const std::uint32_t bits = std::uint32_t(r.read_bits(32));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline void write_point(core::BitWriter& w, const float point[3]) {
  for (int i = 0; i < 3; ++i) {
    write_float(w, point[i]);
  }
}

inline void read_point(core::BitReader& r, float point[3]) {
  for (int i = 0; i < 3; ++i) {
    point[i] = read_float(r);
  }
}

inline bool valid_point(const float point[3]) {
  for (int i = 0; i < 3; ++i) {
    if (!std::isfinite(point[i]) || std::fabs(point[i]) > kMaxRayCoordinate) {
      return false;
    }
  }
  return true;
}

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_bits(text.size(), 8);
  w.write_byte_run(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

inline bool read_text(core::BitReader& r, std::string& text, std::size_t most) {
  const std::size_t length = std::size_t(r.read_bits(8));
  if (r.overrun() || length > most) {
    return false;
  }
  text.resize(length);
  r.read_byte_run(reinterpret_cast<std::uint8_t*>(text.data()), length);
  return !r.overrun();
}

inline bool finite_all(const float* values, int count) {
  for (int i = 0; i < count; ++i) {
    if (!std::isfinite(values[i])) {
      return false;
    }
  }
  return true;
}

inline bool valid_hit(const WorldRayHit& hit) {
  return valid_point(hit.position) && valid_point(hit.normal) && std::isfinite(hit.distance) &&
         hit.surface.size() <= kMaxSurfaceName && std::uint8_t(hit.kind) < kRayHitKindCount &&
         hit.model.size() <= kMaxRayText && hit.guid.size() <= kMaxRayText &&
         hit.name.size() <= kMaxRayText && hit.entity_class.size() <= kMaxRayText &&
         std::uint8_t(hit.object) < kRayObjectCount &&
         (hit.object == RayObject::None ||
          (valid_point(hit.object_at) && finite_all(hit.object_rotation, 4) &&
           finite_all(hit.object_scale, 3)));
}

// One hit as WORLD_RAYCAST_REPLY and WEAPON_HIT carry it (the hit is valid_hit).
inline void write_hit(core::BitWriter& w, const WorldRayHit& hit) {
  write_point(w, hit.position);
  write_point(w, hit.normal);
  write_float(w, hit.distance);
  w.write_bool(hit.terrain);
  w.write_bits(hit.entity, 32);
  w.write_bits(hit.surface.size(), 8);
  w.write_byte_run(reinterpret_cast<const std::uint8_t*>(hit.surface.data()), hit.surface.size());
  w.write_bits(std::uint8_t(hit.kind), 8);
  write_text(w, hit.model);
  write_text(w, hit.guid);
  write_text(w, hit.name);
  write_text(w, hit.entity_class);
  w.write_bits(std::uint8_t(hit.object), 8);
  if (hit.object != RayObject::None) {
    write_point(w, hit.object_at);
    for (int i = 0; i < 4; ++i) {
      write_float(w, hit.object_rotation[i]);
    }
    write_point(w, hit.object_scale);
  }
}

// One hit as write_hit wrote it; false when it is cut short or not a valid hit.
inline bool read_hit(core::BitReader& r, WorldRayHit& hit) {
  read_point(r, hit.position);
  read_point(r, hit.normal);
  hit.distance = read_float(r);
  hit.terrain = r.read_bool();
  hit.entity = std::uint32_t(r.read_bits(32));
  const std::size_t length = std::size_t(r.read_bits(8));
  if (r.overrun() || length > kMaxSurfaceName) {
    return false;
  }
  hit.surface.resize(length);
  r.read_byte_run(reinterpret_cast<std::uint8_t*>(hit.surface.data()), length);
  const std::uint8_t kind = std::uint8_t(r.read_bits(8));
  if (r.overrun() || kind >= kRayHitKindCount || !read_text(r, hit.model, kMaxRayText) ||
      !read_text(r, hit.guid, kMaxRayText) || !read_text(r, hit.name, kMaxRayText) ||
      !read_text(r, hit.entity_class, kMaxRayText)) {
    return false;
  }
  hit.kind = RayHitKind(kind);
  const std::uint8_t object = std::uint8_t(r.read_bits(8));
  if (r.overrun() || object >= kRayObjectCount) {
    return false;
  }
  hit.object = RayObject(object);
  if (hit.object != RayObject::None) {
    read_point(r, hit.object_at);
    for (int i = 0; i < 4; ++i) {
      hit.object_rotation[i] = read_float(r);
    }
    read_point(r, hit.object_scale);
  }
  return !r.overrun() && valid_hit(hit);
}

}  // namespace world_ray_detail

// A question worth asking: a ray with both ends in the world, of some length and at most
// kMaxRayLength, asking for 1 hit (First) or 1 .. kMaxRayHits (All); or a centre in the world with
// a radius of more than 0 and at most kMaxBodyRadius (Bodies); or one of at most
// kMaxEntityRadius asking for 1 .. kMaxLevelEntities (Entities).
inline bool valid_ray(const WorldRayRequest& ray) {
  if (std::uint8_t(ray.query) >= kRayQueryCount) {
    return false;
  }
  if (ray.query == RayQuery::Entities) {
    return world_ray_detail::valid_point(ray.center) && std::isfinite(ray.radius) &&
           ray.radius > 0.0f && ray.radius <= kMaxEntityRadius && ray.max_hits >= 1 &&
           ray.max_hits <= kMaxLevelEntities;
  }
  if (ray.query == RayQuery::Bodies) {
    return world_ray_detail::valid_point(ray.center) && std::isfinite(ray.radius) &&
           ray.radius > 0.0f && ray.radius <= kMaxBodyRadius;
  }
  if (!world_ray_detail::valid_point(ray.from) || !world_ray_detail::valid_point(ray.to) ||
      std::uint8_t(ray.mode) >= kRayModeCount) {
    return false;
  }
  const bool hits_ok = ray.query == RayQuery::First
                           ? ray.max_hits == 1
                           : ray.max_hits >= 1 && ray.max_hits <= kMaxRayHits;
  const float dx = ray.to[0] - ray.from[0];
  const float dy = ray.to[1] - ray.from[1];
  const float dz = ray.to[2] - ray.from[2];
  const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
  return hits_ok && length > 0.0f && length <= kMaxRayLength;
}

// WORLD_RAYCAST; empty for a question that is not valid_ray.
inline std::vector<std::uint8_t> encode_world_raycast(const WorldRayRequest& ray) {
  if (!valid_ray(ray)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWorldRaycastTag, 8);
  w.write_bits(ray.request, 32);
  w.write_bits(std::uint8_t(ray.query), 8);
  if (ray.query == RayQuery::Bodies) {
    world_ray_detail::write_point(w, ray.center);
    world_ray_detail::write_float(w, ray.radius);
  } else if (ray.query == RayQuery::Entities) {
    world_ray_detail::write_point(w, ray.center);
    world_ray_detail::write_float(w, ray.radius);
    w.write_bits(ray.max_hits, 8);
  } else {
    world_ray_detail::write_point(w, ray.from);
    world_ray_detail::write_point(w, ray.to);
    w.write_bits(std::uint8_t(ray.mode), 8);
    w.write_bits(ray.max_hits, 8);
  }
  return w.bytes();
}

inline bool decode_world_raycast(const std::vector<std::uint8_t>& p, WorldRayRequest& ray) {
  if (p.empty() || p[0] != kWorldRaycastTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  WorldRayRequest decoded;
  decoded.request = std::uint32_t(r.read_bits(32));
  const std::uint8_t query = std::uint8_t(r.read_bits(8));
  if (r.overrun() || query >= kRayQueryCount) {
    return false;
  }
  decoded.query = RayQuery(query);
  if (decoded.query == RayQuery::Bodies) {
    decoded.max_hits = 1;
    world_ray_detail::read_point(r, decoded.center);
    decoded.radius = world_ray_detail::read_float(r);
  } else if (decoded.query == RayQuery::Entities) {
    world_ray_detail::read_point(r, decoded.center);
    decoded.radius = world_ray_detail::read_float(r);
    decoded.max_hits = std::uint8_t(r.read_bits(8));
  } else {
    world_ray_detail::read_point(r, decoded.from);
    world_ray_detail::read_point(r, decoded.to);
    const std::uint8_t mode = std::uint8_t(r.read_bits(8));
    if (mode >= kRayModeCount) {
      return false;
    }
    decoded.mode = RayMode(mode);
    decoded.max_hits = std::uint8_t(r.read_bits(8));
  }
  if (r.overrun() || r.bits_remaining() >= 8 || !valid_ray(decoded)) {
    return false;
  }
  ray = decoded;
  return true;
}

// The most hits a reply to the query carries.
inline std::size_t most_hits(RayQuery query) {
  if (query == RayQuery::First) {
    return 1;
  }
  return query == RayQuery::Entities ? kMaxLevelEntities : kMaxRayHits;
}

// WORLD_RAYCAST_REPLY; empty for an answer that cannot travel: a found status without anything
// found, more hits than kMaxRayHits or bodies than kMaxBodiesInRadius, or numbers out of the
// world.
inline std::vector<std::uint8_t> encode_world_raycast_reply(const WorldRayReply& reply) {
  if (std::uint8_t(reply.status) >= kRayStatusCount ||
      std::uint8_t(reply.query) >= kRayQueryCount) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWorldRaycastReplyTag, 8);
  w.write_bits(reply.request, 32);
  w.write_bits(std::uint8_t(reply.query), 8);
  w.write_bits(std::uint8_t(reply.status), 8);
  if (reply.status != RayStatus::Hit) {
    return w.bytes();
  }
  if (reply.query == RayQuery::Bodies) {
    if (reply.bodies.empty() || reply.bodies.size() > kMaxBodiesInRadius) {
      return {};
    }
    w.write_bits(reply.bodies.size(), 8);
    for (const WorldBody& body : reply.bodies) {
      if (!world_ray_detail::valid_point(body.position)) {
        return {};
      }
      w.write_bits(body.entity, 32);
      world_ray_detail::write_point(w, body.position);
    }
    return w.bytes();
  }
  if (reply.hits.empty() || reply.hits.size() > most_hits(reply.query)) {
    return {};
  }
  w.write_bits(reply.hits.size(), 8);
  for (const WorldRayHit& hit : reply.hits) {
    if (!world_ray_detail::valid_hit(hit)) {
      return {};
    }
    world_ray_detail::write_hit(w, hit);
  }
  return w.bytes();
}

inline bool decode_world_raycast_reply(const std::vector<std::uint8_t>& p, WorldRayReply& reply) {
  if (p.empty() || p[0] != kWorldRaycastReplyTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  WorldRayReply decoded;
  decoded.request = std::uint32_t(r.read_bits(32));
  const std::uint8_t query = std::uint8_t(r.read_bits(8));
  const std::uint8_t status = std::uint8_t(r.read_bits(8));
  if (r.overrun() || query >= kRayQueryCount || status >= kRayStatusCount) {
    return false;
  }
  decoded.query = RayQuery(query);
  decoded.status = RayStatus(status);
  if (decoded.status == RayStatus::Hit && decoded.query == RayQuery::Bodies) {
    const std::size_t count = std::size_t(r.read_bits(8));
    if (count == 0 || count > kMaxBodiesInRadius) {
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      WorldBody body;
      body.entity = std::uint32_t(r.read_bits(32));
      world_ray_detail::read_point(r, body.position);
      if (r.overrun() || !world_ray_detail::valid_point(body.position)) {
        return false;
      }
      decoded.bodies.push_back(body);
    }
  } else if (decoded.status == RayStatus::Hit) {
    const std::size_t count = std::size_t(r.read_bits(8));
    if (count == 0 || count > most_hits(decoded.query)) {
      return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
      WorldRayHit hit;
      if (!world_ray_detail::read_hit(r, hit)) {
        return false;
      }
      decoded.hits.push_back(std::move(hit));
    }
  }
  if (r.bits_remaining() >= 8) {
    return false;
  }
  reply = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
