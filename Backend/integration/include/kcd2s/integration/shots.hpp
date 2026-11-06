#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/world_ray.hpp"

// A shot with a bow or crossbow, on the control channel (0).
//
//   SHOT       : the shooter's client -> server, once per projectile its game launches: the
//                ammunition's class (its 16 bytes as the game keeps them), where the projectile
//                leaves, its direction and speed, and the game's kind of shot.
//   SHOT_RELAY : server -> every other client in the world, with the shooter's network id: each
//                launches the same projectile from the shooter's replica, and the game of the
//                player it hits works the hit out (as for a melee blow).
//   SHOT_LANDED: the shooter's client -> server, once one of its projectiles lies in the world
//                (the game makes a pickable of it where it lands): its class and where it lies.
//                The server lays it as its own ground stack, which everyone sees and one player
//                takes up; every client takes its own copy of the projectile out of its world.
//   NPC_SHOT   : an NPC's simulator -> server, once per projectile the NPC's bow fires
//                (Npc.attack with the bow): the NPC's network id and the shot. The server takes
//                it only from that NPC's simulator and relays it as SHOT_RELAY with the NPC as the
//                shooter, so every other client fires it from the NPC's body there.
namespace kcd2s::integration {

inline constexpr std::uint8_t kShotTag = 40;
inline constexpr std::uint8_t kShotRelayTag = 41;
inline constexpr std::uint8_t kShotLandedTag = 42;
inline constexpr std::uint8_t kNpcShotTag = 145;
// Faster than any bow or crossbow the game has.
inline constexpr float kMaxShotSpeed = 400.0f;
inline constexpr float kMaxShotCoordinate = 100000.0f;

struct Shot {
  std::array<std::uint8_t, 16> ammo{};
  float position[3] = {};
  float direction[3] = {};
  float speed = 0;
  std::uint8_t kind = 0;

  friend bool operator==(const Shot& a, const Shot& b) {
    return a.ammo == b.ammo && std::memcmp(a.position, b.position, sizeof(a.position)) == 0 &&
           std::memcmp(a.direction, b.direction, sizeof(a.direction)) == 0 && a.speed == b.speed &&
           a.kind == b.kind;
  }
};

// A projectile lying where it landed: its class, position and rotation (x, y, z, w).
struct ShotLanded {
  std::array<std::uint8_t, 16> ammo{};
  float position[3] = {};
  float rotation[4] = {0, 0, 0, 1};
  // Stuck in what it hit (a target, a wall, a tree): it stays there, without falling.
  bool stuck = false;
  // What it struck where it lies (playerMissileHit): the shooter's game
  // traces a short ray along it there and describes the hit as a World.raycast hit does
  // (world_ray.hpp); none when the ray met nothing.
  std::optional<WorldRayHit> hit;

  friend bool operator==(const ShotLanded& a, const ShotLanded& b) {
    return a.ammo == b.ammo && std::memcmp(a.position, b.position, sizeof(a.position)) == 0 &&
           std::memcmp(a.rotation, b.rotation, sizeof(a.rotation)) == 0 && a.stuck == b.stuck &&
           a.hit == b.hit;
  }
};

namespace shots_detail {

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const auto bits = std::uint32_t(r.read_bits(32));
  float value = 0;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// A position inside the world, a direction of about unit length and a speed the game can shoot.
inline bool valid(const Shot& shot) {
  float length = 0;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(shot.position[axis]) ||
        std::fabs(shot.position[axis]) > kMaxShotCoordinate ||
        !std::isfinite(shot.direction[axis])) {
      return false;
    }
    length += shot.direction[axis] * shot.direction[axis];
  }
  return std::fabs(length - 1.0f) < 0.05f && std::isfinite(shot.speed) && shot.speed > 0.0f &&
         shot.speed <= kMaxShotSpeed;
}

inline void write_shot(core::BitWriter& w, const Shot& shot) {
  for (const std::uint8_t byte : shot.ammo) {
    w.write_bits(byte, 8);
  }
  for (const float value : shot.position) {
    write_float(w, value);
  }
  for (const float value : shot.direction) {
    write_float(w, value);
  }
  write_float(w, shot.speed);
  w.write_bits(shot.kind, 8);
}

inline void read_shot(core::BitReader& r, Shot& shot) {
  for (std::uint8_t& byte : shot.ammo) {
    byte = std::uint8_t(r.read_bits(8));
  }
  for (float& value : shot.position) {
    value = read_float(r);
  }
  for (float& value : shot.direction) {
    value = read_float(r);
  }
  shot.speed = read_float(r);
  shot.kind = std::uint8_t(r.read_bits(8));
}

// A position inside the world and a rotation of about unit length.
inline bool valid_landing(const ShotLanded& landed) {
  float length = 0;
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(landed.position[axis]) ||
        std::fabs(landed.position[axis]) > kMaxShotCoordinate) {
      return false;
    }
  }
  for (const float part : landed.rotation) {
    if (!std::isfinite(part)) {
      return false;
    }
    length += part * part;
  }
  return std::fabs(length - 1.0f) < 0.05f;
}

}  // namespace shots_detail

// SHOT: [tag][16 bytes ammunition][3 x f32 position][3 x f32 direction][f32 speed][u8 kind]; empty
// for a shot that cannot travel.
inline std::vector<std::uint8_t> encode_shot(const Shot& shot) {
  if (!shots_detail::valid(shot)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kShotTag, 8);
  shots_detail::write_shot(w, shot);
  return w.bytes();
}

inline bool decode_shot(const std::vector<std::uint8_t>& p, Shot& shot) {
  if (p.empty() || p[0] != kShotTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  Shot decoded;
  shots_detail::read_shot(r, decoded);
  if (r.overrun() || r.bits_remaining() != 0 || !shots_detail::valid(decoded)) {
    return false;
  }
  shot = decoded;
  return true;
}

// SHOT_RELAY: [tag][varuint shooter][the shot as SHOT carries it].
inline std::vector<std::uint8_t> encode_shot_relay(std::uint32_t shooter, const Shot& shot) {
  if (shooter == 0 || !shots_detail::valid(shot)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kShotRelayTag, 8);
  w.write_varuint(shooter);
  shots_detail::write_shot(w, shot);
  return w.bytes();
}

inline bool decode_shot_relay(const std::vector<std::uint8_t>& p, std::uint32_t& shooter,
                              Shot& shot) {
  if (p.empty() || p[0] != kShotRelayTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t id = r.read_varuint();
  Shot decoded;
  shots_detail::read_shot(r, decoded);
  if (r.overrun() || r.bits_remaining() != 0 || id == 0 || id > 0xFFFFFFFFu ||
      !shots_detail::valid(decoded)) {
    return false;
  }
  shooter = std::uint32_t(id);
  shot = decoded;
  return true;
}

// NPC_SHOT: [tag][varuint npc][the shot as SHOT carries it].
inline std::vector<std::uint8_t> encode_npc_shot(std::uint32_t npc, const Shot& shot) {
  if (npc == 0 || !shots_detail::valid(shot)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kNpcShotTag, 8);
  w.write_varuint(npc);
  shots_detail::write_shot(w, shot);
  return w.bytes();
}

inline bool decode_npc_shot(const std::vector<std::uint8_t>& p, std::uint32_t& npc, Shot& shot) {
  if (p.empty() || p[0] != kNpcShotTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t id = r.read_varuint();
  Shot decoded;
  shots_detail::read_shot(r, decoded);
  if (r.overrun() || r.bits_remaining() != 0 || id == 0 || id > 0xFFFFFFFFu ||
      !shots_detail::valid(decoded)) {
    return false;
  }
  npc = std::uint32_t(id);
  shot = decoded;
  return true;
}

// SHOT_LANDED: [tag][16 bytes ammunition][3 x f32 position][4 x f32 rotation][u8 stuck 0/1]
// [u8 hit 0/1, with 1 the hit as WORLD_RAYCAST_REPLY carries one]; empty for a landing outside
// the world or with no rotation. A hit that cannot travel is left out.
inline std::vector<std::uint8_t> encode_shot_landed(const ShotLanded& landed) {
  if (!shots_detail::valid_landing(landed)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kShotLandedTag, 8);
  for (const std::uint8_t byte : landed.ammo) {
    w.write_bits(byte, 8);
  }
  for (const float value : landed.position) {
    shots_detail::write_float(w, value);
  }
  for (const float value : landed.rotation) {
    shots_detail::write_float(w, value);
  }
  w.write_bits(landed.stuck ? 1 : 0, 8);
  const bool hit = landed.hit && world_ray_detail::valid_hit(*landed.hit);
  w.write_bits(hit ? 1 : 0, 8);
  if (hit) {
    world_ray_detail::write_hit(w, *landed.hit);
  }
  return w.bytes();
}

inline bool decode_shot_landed(const std::vector<std::uint8_t>& p, ShotLanded& landed) {
  if (p.empty() || p[0] != kShotLandedTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  ShotLanded decoded;
  for (std::uint8_t& byte : decoded.ammo) {
    byte = std::uint8_t(r.read_bits(8));
  }
  for (float& value : decoded.position) {
    value = shots_detail::read_float(r);
  }
  for (float& value : decoded.rotation) {
    value = shots_detail::read_float(r);
  }
  const auto stuck = r.read_bits(8);
  const auto hit = r.read_bits(8);
  if (r.overrun() || stuck > 1 || hit > 1 || !shots_detail::valid_landing(decoded)) {
    return false;
  }
  if (hit == 1) {
    WorldRayHit struck;
    if (!world_ray_detail::read_hit(r, struck)) {
      return false;
    }
    decoded.hit = std::move(struck);
  }
  if (r.bits_remaining() >= 8) {
    return false;
  }
  decoded.stuck = stuck == 1;
  landed = decoded;
  return true;
}

}  // namespace kcd2s::integration
