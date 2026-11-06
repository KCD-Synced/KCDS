#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "kcd2s/core/bits.hpp"

// What lies on an alchemy table the player brews at. On the control channel (0).
//   CRAFT_TABLE : the player's own client -> server, whenever it changes: every item of the
//                 game's running alchemy minigame by the table position it lies at (the game's
//                 21 vessels and places, 0 to 20), its item class and whether it is a base
//                 liquid, milled or distilled. Empty when nothing lies there. The server keeps it
//                 for Crafting.session's resources.
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kCraftTableTag = 139;

// The table's positions, the most items one message carries, and how often a client may send.
inline constexpr std::uint8_t kCraftTablePositions = 21;
inline constexpr std::size_t kMaxCraftTableItems = 64;
inline constexpr unsigned kMaxCraftTablesPerSecond = 4;

struct CraftTableItem {
  std::uint8_t position = 0;
  std::array<std::uint8_t, 16> item{};  // the item class GUID, in the game's byte order
  bool base = false;
  bool milled = false;
  bool distilled = false;

  bool operator==(const CraftTableItem&) const = default;
};

struct CraftTable {
  std::vector<CraftTableItem> items;

  bool operator==(const CraftTable&) const = default;
};

// CRAFT_TABLE: [tag][varuint n][n x (u8 position, 16 item, u8 flags: 1 base, 2 milled,
// 4 distilled)]. Empty for a table that cannot travel.
inline std::vector<std::uint8_t> encode_craft_table(const CraftTable& table) {
  if (table.items.size() > kMaxCraftTableItems) {
    return {};
  }
  for (const CraftTableItem& item : table.items) {
    if (item.position >= kCraftTablePositions) {
      return {};
    }
  }
  core::BitWriter w;
  w.write_bits(kCraftTableTag, 8);
  w.write_varuint(table.items.size());
  for (const CraftTableItem& item : table.items) {
    w.write_bits(item.position, 8);
    w.write_bytes(item.item.data(), item.item.size());
    const unsigned flags =
        (item.base ? 1u : 0u) | (item.milled ? 2u : 0u) | (item.distilled ? 4u : 0u);
    w.write_bits(flags, 8);
  }
  return w.bytes();
}

inline bool decode_craft_table(const std::vector<std::uint8_t>& p, CraftTable& table) {
  if (p.empty() || p[0] != kCraftTableTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kMaxCraftTableItems) {
    return false;
  }
  CraftTable decoded;
  decoded.items.resize(count);
  for (CraftTableItem& item : decoded.items) {
    item.position = std::uint8_t(r.read_bits(8));
    for (std::uint8_t& byte : item.item) {
      byte = std::uint8_t(r.read_bits(8));
    }
    const std::uint64_t flags = r.read_bits(8);
    if (r.overrun() || item.position >= kCraftTablePositions || flags > 7) {
      return false;
    }
    item.base = (flags & 1) != 0;
    item.milled = (flags & 2) != 0;
    item.distilled = (flags & 4) != 0;
  }
  if (r.bits_remaining() != 0) {
    return false;
  }
  table = decoded;
  return true;
}

}  // namespace kcd2s::integration
