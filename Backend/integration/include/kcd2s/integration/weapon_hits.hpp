#pragma once
#include <cstdint>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/world_ray.hpp"

// A player's swing meeting something solid that is not a body: the
// client's weaponHit event and the server's playerWeaponHit. The player's own game finds the
// contact (Client/runtime/weapon_hits/) and tells the server once per thing per attack.
//
//   WEAPON_HIT : the player's own client -> server, on the control channel (0):
//                [tag][hit], the hit as WORLD_RAYCAST_REPLY carries one (world_ray.hpp), its
//                distance measured from the player's feet. entity is always 0: a body is never
//                reported.
//
// The server takes it as that player's word, checked only to lie within kWeaponHitReach of where
// the server has the player, and at most kMaxWeaponHitsPerSecond from one player.
namespace kcd2s::integration {

inline constexpr std::uint8_t kWeaponHitTag = 159;

// How far from the player a reported hit may lie (metres), and how many the server takes from one
// player in a second.
inline constexpr float kWeaponHitReach = 8.0f;
inline constexpr unsigned kMaxWeaponHitsPerSecond = 16;

// Empty when the hit is not valid (world_ray.hpp's valid_hit) or names a body.
inline std::vector<std::uint8_t> encode_weapon_hit(const WorldRayHit& hit) {
  if (!world_ray_detail::valid_hit(hit) || hit.entity != 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWeaponHitTag, 8);
  world_ray_detail::write_hit(w, hit);
  return w.bytes();
}

inline bool decode_weapon_hit(const std::vector<std::uint8_t>& p, WorldRayHit& hit) {
  if (p.empty() || p[0] != kWeaponHitTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  WorldRayHit decoded;
  if (!world_ray_detail::read_hit(r, decoded) || decoded.entity != 0 || r.bits_remaining() >= 8) {
    return false;
  }
  hit = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
