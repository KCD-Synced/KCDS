#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Level edits (the server's LevelEdit): an object the level itself placed (a brush,
// or a level entity) hidden, relocated, or both, in one dimension.
//
//   LEVEL_EDIT_APPLY  : server -> a ready player's client, on the control channel (0): the whole
//                       edit `id` (a later change restates it).
//                       [tag][varuint id][u8 kind: 1 brush, 2 entity, 3 layer][varuint
//                       length][name] [u64 guid][f32 x3 at][u8 flags: 1 hidden, 2 position, 4
//                       rotation, 8 scale] [f32 x3 position][f32 x4 rotation x, y, z, w][f32 x3
//                       scale], each of the last three only when its flag is set.
//   LEVEL_EDIT_REMOVE : server -> a ready player's client: the edit `id` is gone, the object goes
//                       back to where the level put it and shows again. [tag][varuint id]
//
// A brush is found by its mesh path (`name`, objects/...cgf) within kLevelEditMatchMetres of
// `at`, a level entity by its EntityGuid (`guid`, `at` is where the level put it). The server
// sends an edit to the players of its dimension (or every player, for the global world), and
// sends every edit again when a player's world gets ready or the player changes world.
//
// Client/runtime/level/ implements the client side.
namespace kcd2s::integration {

inline constexpr std::uint8_t kLevelEditApplyTag = 94;
inline constexpr std::uint8_t kLevelEditRemoveTag = 95;

inline constexpr std::size_t kMaxLevelEditNameLength = 255;
// Two brushes of the same mesh closer than this are one object (the server's own match, 5 cm).
inline constexpr float kLevelEditMatchMetres = 0.05f;
inline constexpr float kLevelEditWorldLimit = 1e6f;
inline constexpr float kLevelEditScaleMin = 0.01f;
inline constexpr float kLevelEditScaleMax = 100.0f;

// A layer edit switches one of the level's story layers (its full name in `name`): `hidden` turns
// it off, otherwise it is on; it moves nothing and its `at` is only where the layer is.
enum class LevelEditKind : std::uint8_t { Brush = 1, Entity = 2, Layer = 3 };

struct LevelEdit {
  std::uint32_t id = 0;
  LevelEditKind kind = LevelEditKind::Brush;
  std::string name;        // a brush's mesh path; an entity's name (may be empty); a layer's name
  std::uint64_t guid = 0;  // an entity's EntityGuid; 0 for a brush
  float at[3] = {0.0f, 0.0f, 0.0f};
  bool hidden = false;
  bool has_position = false;
  bool has_rotation = false;
  bool has_scale = false;
  float position[3] = {0.0f, 0.0f, 0.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};  // x, y, z, w
  float scale[3] = {1.0f, 1.0f, 1.0f};

  friend bool operator==(const LevelEdit& a, const LevelEdit& b) {
    const auto same = [](const float* x, const float* y, int count) {
      for (int i = 0; i < count; ++i) {
        if (x[i] != y[i]) {
          return false;
        }
      }
      return true;
    };
    return a.id == b.id && a.kind == b.kind && a.name == b.name && a.guid == b.guid &&
           same(a.at, b.at, 3) && a.hidden == b.hidden && a.has_position == b.has_position &&
           a.has_rotation == b.has_rotation && a.has_scale == b.has_scale &&
           same(a.position, b.position, 3) && same(a.rotation, b.rotation, 4) &&
           same(a.scale, b.scale, 3);
  }
};

// An EntityGuid as the server writes it: 16 lower-case hex digits, most significant first.
// The 16 hex digits of an entity key in the level files' order (parse_level_edit_guid, below).
inline std::string level_edit_guid_text(std::uint64_t guid) {
  std::uint64_t ordered =
      ((guid & 0xFFFFFFFFull) << 32) | (((guid >> 32) & 0xFFFF) << 16) | ((guid >> 48) & 0xFFFF);
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text(16, '0');
  for (int i = 15; i >= 0; --i) {
    text[std::size_t(i)] = kDigits[ordered & 0xF];
    ordered >>= 4;
  }
  return text;
}

// An EntityGuid as the level files write it, "10cbd99b-495b-4d4d" (with or without the dashes),
// as the key the entity system holds: its parts 0x10cbd99b, 0x495b and 0x4d4d laid out in memory
// in that order, 0x4d4d495b10cbd99b (read through the entity's GetGUID).
inline bool parse_level_edit_guid(const std::string& text, std::uint64_t& guid) {
  std::string digits;
  for (const char c : text) {
    if (c != '-') {
      digits.push_back(c);
    }
  }
  if (digits.size() != 16) {
    return false;
  }
  std::uint64_t value = 0;
  for (const char c : digits) {
    value <<= 4;
    if (c >= '0' && c <= '9') {
      value |= std::uint64_t(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      value |= std::uint64_t(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      value |= std::uint64_t(c - 'A' + 10);
    } else {
      return false;
    }
  }
  const std::uint64_t part1 = value >> 32;
  const std::uint64_t part2 = (value >> 16) & 0xFFFF;
  const std::uint64_t part3 = value & 0xFFFF;
  guid = part1 | (part2 << 32) | (part3 << 48);
  return true;
}

namespace level_edit_detail {

inline void write_float(core::BitWriter& w, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  w.write_bits(bits, 32);
}

inline float read_float(core::BitReader& r) {
  const auto bits = std::uint32_t(r.read_bits(32));
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

inline bool in_world(const float* point) {
  for (int axis = 0; axis < 3; ++axis) {
    if (!std::isfinite(point[axis]) || std::fabs(point[axis]) > kLevelEditWorldLimit) {
      return false;
    }
  }
  return true;
}

inline bool valid(const LevelEdit& edit) {
  if (edit.id == 0 || edit.name.size() > kMaxLevelEditNameLength || !in_world(edit.at)) {
    return false;
  }
  if (edit.kind == LevelEditKind::Brush) {
    if (edit.name.empty() || edit.guid != 0) {
      return false;
    }
  } else if (edit.kind == LevelEditKind::Entity) {
    if (edit.guid == 0) {
      return false;
    }
  } else if (edit.kind == LevelEditKind::Layer) {
    if (edit.name.empty() || edit.guid != 0 || edit.has_position || edit.has_rotation ||
        edit.has_scale) {
      return false;
    }
  } else {
    return false;
  }
  if (edit.has_position && !in_world(edit.position)) {
    return false;
  }
  if (edit.has_rotation) {
    for (const float component : edit.rotation) {
      if (!std::isfinite(component)) {
        return false;
      }
    }
  }
  if (edit.has_scale) {
    for (const float size : edit.scale) {
      if (!(size >= kLevelEditScaleMin && size <= kLevelEditScaleMax)) {
        return false;
      }
    }
  }
  return true;
}

}  // namespace level_edit_detail

// Empty when the edit cannot travel.
inline std::vector<std::uint8_t> encode_level_edit(const LevelEdit& edit) {
  if (!level_edit_detail::valid(edit)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kLevelEditApplyTag, 8);
  w.write_varuint(edit.id);
  w.write_bits(std::uint8_t(edit.kind), 8);
  w.write_varuint(edit.name.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(edit.name.data()), edit.name.size());
  w.write_bits(edit.guid & 0xFFFFFFFFu, 32);
  w.write_bits(edit.guid >> 32, 32);
  for (const float value : edit.at) {
    level_edit_detail::write_float(w, value);
  }
  const unsigned flags = (edit.hidden ? 1u : 0u) | (edit.has_position ? 2u : 0u) |
                         (edit.has_rotation ? 4u : 0u) | (edit.has_scale ? 8u : 0u);
  w.write_bits(flags, 8);
  if (edit.has_position) {
    for (const float value : edit.position) {
      level_edit_detail::write_float(w, value);
    }
  }
  if (edit.has_rotation) {
    for (const float value : edit.rotation) {
      level_edit_detail::write_float(w, value);
    }
  }
  if (edit.has_scale) {
    for (const float value : edit.scale) {
      level_edit_detail::write_float(w, value);
    }
  }
  return w.bytes();
}

inline bool decode_level_edit(const std::vector<std::uint8_t>& p, LevelEdit& edit) {
  if (p.empty() || p[0] != kLevelEditApplyTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  LevelEdit decoded;
  const std::uint64_t id = r.read_varuint();
  decoded.kind = LevelEditKind(r.read_bits(8));
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || id == 0 || id > 0xFFFFFFFFu || length > kMaxLevelEditNameLength ||
      length * 8 > r.bits_remaining()) {
    return false;
  }
  decoded.id = std::uint32_t(id);
  decoded.name.resize(std::size_t(length));
  for (char& c : decoded.name) {
    c = char(r.read_bits(8));
  }
  const std::uint64_t low = r.read_bits(32);
  const std::uint64_t high = r.read_bits(32);
  decoded.guid = low | (high << 32);
  for (float& value : decoded.at) {
    value = level_edit_detail::read_float(r);
  }
  const std::uint64_t flags = r.read_bits(8);
  if (r.overrun() || flags > 15) {
    return false;
  }
  decoded.hidden = (flags & 1) != 0;
  decoded.has_position = (flags & 2) != 0;
  decoded.has_rotation = (flags & 4) != 0;
  decoded.has_scale = (flags & 8) != 0;
  if (decoded.has_position) {
    for (float& value : decoded.position) {
      value = level_edit_detail::read_float(r);
    }
  }
  if (decoded.has_rotation) {
    for (float& value : decoded.rotation) {
      value = level_edit_detail::read_float(r);
    }
  }
  if (decoded.has_scale) {
    for (float& value : decoded.scale) {
      value = level_edit_detail::read_float(r);
    }
  }
  if (r.overrun() || r.bits_remaining() != 0 || !level_edit_detail::valid(decoded)) {
    return false;
  }
  edit = std::move(decoded);
  return true;
}

inline std::vector<std::uint8_t> encode_level_edit_remove(std::uint32_t id) {
  if (id == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kLevelEditRemoveTag, 8);
  w.write_varuint(id);
  return w.bytes();
}

// What a client heard since its game last took the changes, oldest first: an edit applied (or
// restated), or the edit `edit.id` removed.
struct LevelEditChange {
  bool removed = false;
  LevelEdit edit;
};
// Changes a client holds between two frames (a world's replay of every edit included). 1024 dropped
// the rest of a World Architect export's 2 603 edits on joining (a moved table
// stood beside its copy on the server), and of a project's when the editor opened it again.
inline constexpr std::size_t kMaxLevelEditChanges = 65536;

inline bool decode_level_edit_remove(const std::vector<std::uint8_t>& p, std::uint32_t& id) {
  if (p.empty() || p[0] != kLevelEditRemoveTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || r.bits_remaining() != 0 || value == 0 || value > 0xFFFFFFFFu) {
    return false;
  }
  id = std::uint32_t(value);
  return true;
}

}  // namespace kcd2s::integration
