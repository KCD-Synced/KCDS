#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// The game's own items lying in its level: tankards, tools and everything else the
// level places. Every client loads the same level, so each has its own copy of every such item; the
// server decides who takes one up, once, and the others no longer see it.
//
// An item is named by its key: its class as scripts see it (32 hex digits, hands.hpp's
// item_class_script_text) and where it lies, in whole centimetres: "<class>@<x>,<y>,<z>". Every
// client works the same key out from its own copy.
//
//   WORLD_PICKUP         : a player's client -> server, on the control channel (0): the player's
//                          game would take an item up; the client holds it until the verdict.
//   WORLD_PICKUP_VERDICT : server -> that client: take it up (with the game's own pick-up), or not.
//   WORLD_TAKEN          : server -> clients: the items taken up, gone from every world; `full`
//                          replaces the list a client had (sent when its world is ready).
//   WORLD_ITEMS_ENABLED  : server -> clients: [tag][u8 enabled 0/1]. Off, every item
//                          of the level goes out of every world (World.setLevelItemsEnabled, the
//                          server's level-items.json) and no pick-up is granted; on again, they
//                          come back. A client hides such items (and the ones taken up): a level
//                          item whose entity is removed is made anew by the game.
//   ITEM_NOTICE          : server -> a player's client: units of a class the server put into (or
//                          took out of) the player's inventory, for the game's own notice
//                          ("item received"); the inventory itself travels with the stock.
namespace kcd2s::integration {

inline constexpr std::uint8_t kWorldPickupTag = 37;
inline constexpr std::uint8_t kWorldPickupVerdictTag = 38;
inline constexpr std::uint8_t kWorldTakenTag = 39;
inline constexpr std::uint8_t kItemNoticeTag = 43;
inline constexpr std::uint8_t kWorldItemsEnabledTag = 151;
inline constexpr std::uint8_t kWorldItemsRemovedTag = 170;

inline std::vector<std::uint8_t> encode_world_items_enabled(bool enabled) {
  return {kWorldItemsEnabledTag, std::uint8_t(enabled ? 1 : 0)};
}

inline bool decode_world_items_enabled(const std::vector<std::uint8_t>& in, bool& enabled) {
  if (in.size() != 2 || in[0] != kWorldItemsEnabledTag || in[1] > 1) {
    return false;
  }
  enabled = in[1] == 1;
  return true;
}
inline constexpr std::uint32_t kMaxItemNoticeAmount = 1000000;
inline constexpr std::size_t kMaxWorldItemKeyBytes = 96;
inline constexpr std::size_t kMaxWorldTakenKeys = 256;

namespace world_items_detail {

inline void write_key(core::BitWriter& w, const std::string& key) {
  w.write_varuint(key.size());
  for (const char c : key) {
    w.write_bits(std::uint8_t(c), 8);
  }
}

inline bool read_key(core::BitReader& r, std::string& key) {
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || size == 0 || size > kMaxWorldItemKeyBytes) {
    return false;
  }
  key.resize(std::size_t(size));
  for (char& c : key) {
    c = char(r.read_bits(8));
  }
  return !r.overrun();
}

}  // namespace world_items_detail

// A key's form: 32 hex digits, '@', three whole numbers separated by ','.
inline bool valid_world_item_key(const std::string& key) {
  if (key.size() < 38 || key.size() > kMaxWorldItemKeyBytes || key[32] != '@') {
    return false;
  }
  for (std::size_t i = 0; i < 32; ++i) {
    const char c = key[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
      return false;
    }
  }
  unsigned commas = 0;
  bool digit = false;
  for (std::size_t i = 33; i < key.size(); ++i) {
    const char c = key[i];
    if (c == ',') {
      if (!digit) {
        return false;
      }
      ++commas;
      digit = false;
    } else if (c == '-') {
      if (digit) {
        return false;
      }
    } else if (c >= '0' && c <= '9') {
      digit = true;
    } else {
      return false;
    }
  }
  return commas == 2 && digit;
}

// The key of an item of a class (the GUID's 16 bytes as the game keeps them) lying at a position
// in metres.
inline std::string world_item_key(const std::uint8_t* item_class, const float position[3]) {
  static const char* digits = "0123456789abcdef";
  std::string key;
  for (std::size_t half = 0; half < 2; ++half) {
    for (std::size_t i = 8; i-- > 0;) {
      const std::uint8_t byte = item_class[half * 8 + i];
      key += digits[byte >> 4];
      key += digits[byte & 15];
    }
  }
  key += '@';
  for (int axis = 0; axis < 3; ++axis) {
    if (axis > 0) {
      key += ',';
    }
    key += std::to_string(std::int64_t(std::llround(double(position[axis]) * 100.0)));
  }
  return key;
}

// WORLD_PICKUP: [tag][key]; empty for a key that cannot travel.
inline std::vector<std::uint8_t> encode_world_pickup(const std::string& key) {
  if (!valid_world_item_key(key)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWorldPickupTag, 8);
  world_items_detail::write_key(w, key);
  return w.bytes();
}

inline bool decode_world_pickup(const std::vector<std::uint8_t>& p, std::string& key) {
  if (p.empty() || p[0] != kWorldPickupTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  std::string decoded;
  if (!world_items_detail::read_key(r, decoded) || r.bits_remaining() != 0 ||
      !valid_world_item_key(decoded)) {
    return false;
  }
  key = std::move(decoded);
  return true;
}

// WORLD_PICKUP_VERDICT: [tag][key][u8 1 to take it up, else 0].
inline std::vector<std::uint8_t> encode_world_pickup_verdict(const std::string& key, bool granted) {
  if (!valid_world_item_key(key)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWorldPickupVerdictTag, 8);
  world_items_detail::write_key(w, key);
  w.write_bits(granted ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_world_pickup_verdict(const std::vector<std::uint8_t>& p, std::string& key,
                                        bool& granted) {
  if (p.empty() || p[0] != kWorldPickupVerdictTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  std::string decoded;
  if (!world_items_detail::read_key(r, decoded) || !valid_world_item_key(decoded)) {
    return false;
  }
  const auto value = r.read_bits(8);
  if (r.overrun() || r.bits_remaining() != 0 || value > 1) {
    return false;
  }
  key = std::move(decoded);
  granted = value == 1;
  return true;
}

// The places a script took the level's items away from (World.removeLevelItems):
// a sphere and, when not all zeros, the one item class the place is for. WORLD_ITEMS_REMOVED is
// always the whole list: [tag][varuint count] areas of [3 x f32 centre][f32 radius][16 bytes
// class]. Every client hides the level's own items inside them, and shows them again when the
// list no longer has their place.
inline constexpr std::size_t kMaxRemovedItemAreas = 1024;
inline constexpr float kMaxRemovedItemRadius = 100.0f;

struct RemovedItemArea {
  float center[3] = {0, 0, 0};
  float radius = 0;
  std::array<std::uint8_t, 16> item{};

  friend bool operator==(const RemovedItemArea& a, const RemovedItemArea& b) {
    return std::memcmp(a.center, b.center, sizeof(a.center)) == 0 && a.radius == b.radius &&
           a.item == b.item;
  }
};

inline bool valid_removed_area(const RemovedItemArea& area) {
  for (const float value : area.center) {
    if (!std::isfinite(value) || std::fabs(value) > 100000.0f) {
      return false;
    }
  }
  return std::isfinite(area.radius) && area.radius > 0 && area.radius <= kMaxRemovedItemRadius;
}

// Empty for a list that cannot travel.
inline std::vector<std::uint8_t> encode_world_items_removed(
    const std::vector<RemovedItemArea>& areas) {
  if (areas.size() > kMaxRemovedItemAreas) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kWorldItemsRemovedTag, 8);
  w.write_varuint(areas.size());
  for (const RemovedItemArea& area : areas) {
    if (!valid_removed_area(area)) {
      return {};
    }
    for (const float value : area.center) {
      std::uint32_t bits = 0;
      std::memcpy(&bits, &value, sizeof(bits));
      w.write_bits(bits, 32);
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &area.radius, sizeof(bits));
    w.write_bits(bits, 32);
    for (const std::uint8_t byte : area.item) {
      w.write_bits(byte, 8);
    }
  }
  return w.bytes();
}

inline bool decode_world_items_removed(const std::vector<std::uint8_t>& p,
                                       std::vector<RemovedItemArea>& areas) {
  if (p.empty() || p[0] != kWorldItemsRemovedTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kMaxRemovedItemAreas) {
    return false;
  }
  std::vector<RemovedItemArea> decoded;
  decoded.resize(static_cast<std::size_t>(count));
  for (RemovedItemArea& area : decoded) {
    for (float& value : area.center) {
      const auto bits = std::uint32_t(r.read_bits(32));
      std::memcpy(&value, &bits, sizeof(value));
    }
    const auto bits = std::uint32_t(r.read_bits(32));
    std::memcpy(&area.radius, &bits, sizeof(area.radius));
    for (std::uint8_t& byte : area.item) {
      byte = std::uint8_t(r.read_bits(8));
    }
    if (r.overrun() || !valid_removed_area(area)) {
      return false;
    }
  }
  if (r.bits_remaining() != 0) {
    return false;
  }
  areas = std::move(decoded);
  return true;
}

// The items taken up (WORLD_TAKEN); `full`: the whole list, else ones to add.
struct WorldTaken {
  bool full = false;
  std::vector<std::string> keys;

  friend bool operator==(const WorldTaken& a, const WorldTaken& b) {
    return a.full == b.full && a.keys == b.keys;
  }
};

// WORLD_TAKEN: [tag][u8 full][varuint count][keys]; empty for a list that cannot travel (more than
// kMaxWorldTakenKeys keys: send it in parts, the first full).
inline std::vector<std::uint8_t> encode_world_taken(const WorldTaken& taken) {
  if (taken.keys.size() > kMaxWorldTakenKeys) {
    return {};
  }
  for (const std::string& key : taken.keys) {
    if (!valid_world_item_key(key)) {
      return {};
    }
  }
  core::BitWriter w;
  w.write_bits(kWorldTakenTag, 8);
  w.write_bits(taken.full ? 1 : 0, 8);
  w.write_varuint(taken.keys.size());
  for (const std::string& key : taken.keys) {
    world_items_detail::write_key(w, key);
  }
  return w.bytes();
}

inline bool decode_world_taken(const std::vector<std::uint8_t>& p, WorldTaken& taken) {
  if (p.empty() || p[0] != kWorldTakenTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  WorldTaken decoded;
  const auto full = r.read_bits(8);
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || full > 1 || count > kMaxWorldTakenKeys) {
    return false;
  }
  decoded.full = full == 1;
  decoded.keys.resize(std::size_t(count));
  for (std::string& key : decoded.keys) {
    if (!world_items_detail::read_key(r, key) || !valid_world_item_key(key)) {
      return false;
    }
  }
  if (r.bits_remaining() != 0) {
    return false;
  }
  taken = std::move(decoded);
  return true;
}

// Units of a class (its 16 bytes as the game keeps them) a player gained or lost.
struct ItemNotice {
  std::array<std::uint8_t, 16> item{};
  std::uint32_t amount = 1;
  bool gained = true;

  friend bool operator==(const ItemNotice& a, const ItemNotice& b) {
    return a.item == b.item && a.amount == b.amount && a.gained == b.gained;
  }
};

// ITEM_NOTICE: [tag][16 bytes class][varuint amount][u8 1 gained, 0 lost]; empty for no units.
inline std::vector<std::uint8_t> encode_item_notice(const ItemNotice& notice) {
  if (notice.amount == 0 || notice.amount > kMaxItemNoticeAmount) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kItemNoticeTag, 8);
  for (const std::uint8_t byte : notice.item) {
    w.write_bits(byte, 8);
  }
  w.write_varuint(notice.amount);
  w.write_bits(notice.gained ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_item_notice(const std::vector<std::uint8_t>& p, ItemNotice& notice) {
  if (p.empty() || p[0] != kItemNoticeTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  ItemNotice decoded;
  for (std::uint8_t& byte : decoded.item) {
    byte = std::uint8_t(r.read_bits(8));
  }
  const std::uint64_t amount = r.read_varuint();
  const auto gained = r.read_bits(8);
  if (r.overrun() || r.bits_remaining() != 0 || amount == 0 || amount > kMaxItemNoticeAmount ||
      gained > 1) {
    return false;
  }
  decoded.amount = std::uint32_t(amount);
  decoded.gained = gained == 1;
  notice = decoded;
  return true;
}

}  // namespace kcd2s::integration
