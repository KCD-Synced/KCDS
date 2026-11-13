#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// A dice match as players nearby see it. On the control channel (0).
//   DICE_VIEW (client)  : the thrower's own client -> server: where the dice table's own things
//                         (its cup, a level entity by its EntityGuid key, and its dice, which
//                         each game spawns for itself: their mesh file, material and scale go
//                         along) are in
//                         the thrower's game, and whether each shows, whenever that changes.
//   DICE_VIEW (watcher) : server -> a client near the table that does not play: the same, for its
//                         game to show the table's things so; `ended` puts them back as the level
//                         has them.
//   DICE_TABLES (server): server -> every ready client: whether a resource answers a dice table's
//                         use; while none does, the game's "Play dice" is not offered.
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kDiceViewTag = 140;
inline constexpr std::uint8_t kDiceWatchTag = 141;
inline constexpr std::uint8_t kDiceTablesTag = 142;

// The most things one message carries, how far a thing may be from the world's origin, and how
// often a client may send.
inline constexpr std::size_t kMaxDiceViewThings = 16;
inline constexpr float kDiceViewReach = 100000.0f;
inline constexpr unsigned kMaxDiceViewsPerSecond = 24;
// The longest mesh file or material name a thing names, and the scale a thing may have.
inline constexpr std::size_t kMaxDiceModel = 128;
inline constexpr float kMaxDiceScale = 10.0f;

struct DiceThing {
  std::uint64_t key = 0;  // the entity's EntityGuid key, as the entity system keys it
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};  // x, y, z, w
  bool shown = true;
  // The mesh file a die shows ("objects/....cgf") and the material it is drawn with (empty: the
  // mesh's own), both empty for a level entity; its world scale.
  std::string model;
  std::string material;
  float scale = 1.0f;

  bool operator==(const DiceThing&) const = default;
};

struct DiceView {
  std::uint32_t match = 0;
  std::vector<DiceThing> things;

  bool operator==(const DiceView&) const = default;
};

struct DiceTables {
  bool offered = false;

  bool operator==(const DiceTables&) const = default;
};

struct DiceWatch {
  std::uint32_t match = 0;
  bool ended = false;
  std::vector<DiceThing> things;

  bool operator==(const DiceWatch&) const = default;
};

namespace dice_view_detail {

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const auto bits = static_cast<std::uint32_t>(r.read_bits(32));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// A mesh file or material name: letters, digits and _ / . - only.
inline bool name_valid(const std::string& name) {
  static constexpr char kAllowed[] =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_/.-";
  return name.size() <= kMaxDiceModel && name.find_first_not_of(kAllowed) == std::string::npos;
}

inline void write_name(core::BitWriter& w, const std::string& name) {
  w.write_varuint(name.size());
  for (const char c : name) {
    w.write_bits(static_cast<std::uint8_t>(c), 8);
  }
}

inline bool read_name(core::BitReader& r, std::string& name) {
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || length > kMaxDiceModel) {
    return false;
  }
  name.clear();
  for (std::uint64_t i = 0; i < length; ++i) {
    name.push_back(static_cast<char>(r.read_bits(8)));
  }
  return !r.overrun();
}

inline bool thing_valid(const DiceThing& thing) {
  if (thing.key == 0) {
    return false;
  }
  for (const float value : thing.position) {
    if (!std::isfinite(value) || std::fabs(value) > kDiceViewReach) {
      return false;
    }
  }
  for (const float value : thing.rotation) {
    if (!std::isfinite(value) || std::fabs(value) > 1.01f) {
      return false;
    }
  }
  if (!std::isfinite(thing.scale) || thing.scale <= 0.0f || thing.scale > kMaxDiceScale ||
      !name_valid(thing.model) || !name_valid(thing.material)) {
    return false;
  }
  return true;
}

inline bool write_things(core::BitWriter& w, const std::vector<DiceThing>& things) {
  if (things.size() > kMaxDiceViewThings) {
    return false;
  }
  w.write_varuint(things.size());
  for (const DiceThing& thing : things) {
    if (!thing_valid(thing)) {
      return false;
    }
    w.write_bits(thing.key >> 32, 32);
    w.write_bits(thing.key & 0xFFFFFFFFull, 32);
    for (const float value : thing.position) {
      write_float(w, value);
    }
    for (const float value : thing.rotation) {
      write_float(w, value);
    }
    w.write_bits(thing.shown ? 1 : 0, 8);
    write_float(w, thing.scale);
    write_name(w, thing.model);
    write_name(w, thing.material);
  }
  return true;
}

inline bool read_things(core::BitReader& r, std::vector<DiceThing>& things) {
  const std::uint64_t count = r.read_varuint();
  if (r.overrun() || count > kMaxDiceViewThings) {
    return false;
  }
  things.resize(count);
  for (DiceThing& thing : things) {
    const std::uint64_t high = r.read_bits(32);
    const std::uint64_t low = r.read_bits(32);
    thing.key = (high << 32) | low;
    for (float& value : thing.position) {
      value = read_float(r);
    }
    for (float& value : thing.rotation) {
      value = read_float(r);
    }
    const std::uint64_t shown = r.read_bits(8);
    thing.scale = read_float(r);
    if (!read_name(r, thing.model) || !read_name(r, thing.material) || shown > 1 ||
        !thing_valid(thing)) {
      return false;
    }
    thing.shown = shown == 1;
  }
  return true;
}

}  // namespace dice_view_detail

// DICE_VIEW: [tag][varuint match][things]; a thing: [u64 key][f32 x3][f32 x4][u8 shown][f32 scale]
// [varuint length][model bytes][varuint length][material bytes].
inline std::vector<std::uint8_t> encode_dice_view(const DiceView& view) {
  core::BitWriter w;
  w.write_bits(kDiceViewTag, 8);
  w.write_varuint(view.match);
  if (!dice_view_detail::write_things(w, view.things)) {
    return {};
  }
  return w.bytes();
}

inline bool decode_dice_view(const std::vector<std::uint8_t>& p, DiceView& view) {
  if (p.empty() || p[0] != kDiceViewTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DiceView decoded;
  const std::uint64_t match = r.read_varuint();
  if (r.overrun() || match > 0xFFFFFFFFu || !dice_view_detail::read_things(r, decoded.things) ||
      r.bits_remaining() != 0) {
    return false;
  }
  decoded.match = std::uint32_t(match);
  view = std::move(decoded);
  return true;
}

// DICE_WATCH: [tag][varuint match][u8 ended][things].
inline std::vector<std::uint8_t> encode_dice_watch(const DiceWatch& watch) {
  core::BitWriter w;
  w.write_bits(kDiceWatchTag, 8);
  w.write_varuint(watch.match);
  w.write_bits(watch.ended ? 1 : 0, 8);
  if (!dice_view_detail::write_things(w, watch.things)) {
    return {};
  }
  return w.bytes();
}

inline bool decode_dice_watch(const std::vector<std::uint8_t>& p, DiceWatch& watch) {
  if (p.empty() || p[0] != kDiceWatchTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  DiceWatch decoded;
  const std::uint64_t match = r.read_varuint();
  const std::uint64_t ended = r.read_bits(8);
  if (r.overrun() || match > 0xFFFFFFFFu || ended > 1 ||
      !dice_view_detail::read_things(r, decoded.things) || r.bits_remaining() != 0) {
    return false;
  }
  decoded.match = std::uint32_t(match);
  decoded.ended = ended == 1;
  watch = std::move(decoded);
  return true;
}

// DICE_TABLES: [tag][u8 offered].
inline std::vector<std::uint8_t> encode_dice_tables(const DiceTables& tables) {
  core::BitWriter w;
  w.write_bits(kDiceTablesTag, 8);
  w.write_bits(tables.offered ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_dice_tables(const std::vector<std::uint8_t>& p, DiceTables& tables) {
  if (p.empty() || p[0] != kDiceTablesTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t offered = r.read_bits(8);
  if (r.overrun() || offered > 1 || r.bits_remaining() != 0) {
    return false;
  }
  tables.offered = offered == 1;
  return true;
}

}  // namespace kcd2s::integration
