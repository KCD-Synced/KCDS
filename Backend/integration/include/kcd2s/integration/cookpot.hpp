#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Stew pots: how the pots of a player's dimension look, on their game. On the
// control channel (0).
//   POT_LOOKS : server -> a client: pots by their cauldron (the RuntimePrefab entity the pot's
//               fireplace links as its cauldron, by its level EntityGuid as the level files write
//               it, sixteen hex digits read as one number), whether it is a camp pot (else a home
//               pot: the game's two cauldron prefab categories) and the prefab phase it shows (1
//               empty, 2 to 7 a meal half full or full), and the fire the server holds (0 the
//               game's own, 1 lit, 2 put out; CookPot.setFire).
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kPotLooksTag = 138;

// The most pots in one POT_LOOKS, and the phases a cauldron has.
inline constexpr std::size_t kMaxPotLooks = 512;
inline constexpr std::uint8_t kMinPotPhase = 1;
inline constexpr std::uint8_t kMaxPotPhase = 7;
// The fire a pot's look holds: the game's own, lit or put out.
inline constexpr std::uint8_t kPotFireOwn = 0;
inline constexpr std::uint8_t kPotFireLit = 1;
inline constexpr std::uint8_t kPotFireOut = 2;

struct PotLook {
  std::uint64_t cauldron = 0;
  // The pot's FireplaceSmartObject (its level GUID), whose kettle behaviour holds the fill the
  // game checks before a meal; 0 when the catalog does not name it.
  std::uint64_t fireplace = 0;
  bool camp = false;
  std::uint8_t phase = kMinPotPhase;
  std::uint8_t fire = kPotFireOwn;

  bool operator==(const PotLook&) const = default;
};

struct PotLooks {
  std::vector<PotLook> looks;

  bool operator==(const PotLooks&) const = default;
};

namespace cookpot_detail {

inline bool look_valid(const PotLook& look) {
  return look.cauldron != 0 && look.phase >= kMinPotPhase && look.phase <= kMaxPotPhase &&
         look.fire <= kPotFireOut;
}

}  // namespace cookpot_detail

// POT_LOOKS: [tag][varuint n][n x (u64 cauldron, u64 fireplace, u8 camp, u8 phase, u8 fire)].
// Empty for looks that cannot travel.
inline std::vector<std::uint8_t> encode_pot_looks(const PotLooks& looks) {
  if (looks.looks.empty() || looks.looks.size() > kMaxPotLooks) {
    return {};
  }
  for (const PotLook& look : looks.looks) {
    if (!cookpot_detail::look_valid(look)) {
      return {};
    }
  }
  core::BitWriter w;
  w.write_bits(kPotLooksTag, 8);
  w.write_varuint(looks.looks.size());
  for (const PotLook& look : looks.looks) {
    w.write_bits(look.cauldron >> 32, 32);
    w.write_bits(look.cauldron & 0xFFFFFFFFull, 32);
    w.write_bits(look.fireplace >> 32, 32);
    w.write_bits(look.fireplace & 0xFFFFFFFFull, 32);
    w.write_bits(look.camp ? 1 : 0, 8);
    w.write_bits(look.phase, 8);
    w.write_bits(look.fire, 8);
  }
  return w.bytes();
}

inline bool decode_pot_looks(const std::vector<std::uint8_t>& p, PotLooks& looks) {
  if (p.empty() || p[0] != kPotLooksTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count == 0 || count > kMaxPotLooks) {
    return false;
  }
  PotLooks decoded;
  decoded.looks.resize(count);
  for (PotLook& look : decoded.looks) {
    const std::uint64_t high = r.read_bits(32);
    const std::uint64_t low = r.read_bits(32);
    const std::uint64_t fireplace_high = r.read_bits(32);
    const std::uint64_t fireplace_low = r.read_bits(32);
    const std::uint64_t camp = r.read_bits(8);
    look.cauldron = (high << 32) | low;
    look.fireplace = (fireplace_high << 32) | fireplace_low;
    look.phase = std::uint8_t(r.read_bits(8));
    look.fire = std::uint8_t(r.read_bits(8));
    if (r.overrun() || camp > 1 || !cookpot_detail::look_valid(look)) {
      return false;
    }
    look.camp = camp == 1;
  }
  if (r.bits_remaining() != 0) {
    return false;
  }
  looks = decoded;
  return true;
}

}  // namespace kcd2s::integration
