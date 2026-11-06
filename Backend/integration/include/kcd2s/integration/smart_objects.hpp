#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"

// The smart objects scripts place: SmartObject.spawn. A smart object
// is one of the game's prefabs (Prefabs/*.xml, a runtime template of the game's
// PrefabTemplateLibrary): every client in its dimension builds the whole prefab natively at an
// anchor entity of its own, furniture, solids and the entities its interaction runs on.
//
//   SMART_OBJECT : server -> a ready player's client, on the control channel (0):
//                  [tag][id u32][remove] then, for one placed, [template guid 16 bytes]
//                  [position 3 f32][rotation 4 f32 x, y, z, w][varuint corrections] then each
//                  [place u16][at 3 f32][turn 4 f32 x, y, z, w]
//                  The template is the prefab's Id as the game keeps it (a GUID in the Windows
//                  layout: u32, u16, u16, 8 bytes); `remove` takes the object of that id down.
//                  A correction places the entity at `place` of the build where it stands in the
//                  prefab's frame: one with a brush among its parents, which the game's runtime
//                  build places by its own position only.
//
// Client/runtime/smart_objects/ builds them.
namespace kcd2s::integration {

inline constexpr std::uint8_t kSmartObjectTag = 158;
// The farthest a smart object stands from the world's origin (metres) and still travels.
inline constexpr float kMaxSmartObjectCoordinate = 16384.0f;
// The most corrections one order carries, and the farthest one lies from the prefab's origin.
inline constexpr std::size_t kMaxSmartObjectCorrections = 64;
inline constexpr float kMaxCorrectionOffset = 64.0f;

// Where an entity of the build stands in the prefab's frame, and its turn (x, y, z, w).
struct SmartObjectCorrection {
  std::uint16_t place = 0;
  float at[3] = {0.0f, 0.0f, 0.0f};
  float turn[4] = {0.0f, 0.0f, 0.0f, 1.0f};

  friend bool operator==(const SmartObjectCorrection& a, const SmartObjectCorrection& b) {
    return a.place == b.place && std::memcmp(a.at, b.at, sizeof(a.at)) == 0 &&
           std::memcmp(a.turn, b.turn, sizeof(a.turn)) == 0;
  }
};

struct SmartObjectOrder {
  std::uint32_t id = 0;
  bool remove = false;
  std::array<std::uint8_t, 16> template_guid{};
  float position[3] = {0.0f, 0.0f, 0.0f};
  float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  std::vector<SmartObjectCorrection> corrections;

  friend bool operator==(const SmartObjectOrder& a, const SmartObjectOrder& b) {
    return a.id == b.id && a.remove == b.remove && a.template_guid == b.template_guid &&
           std::memcmp(a.position, b.position, sizeof(a.position)) == 0 &&
           std::memcmp(a.rotation, b.rotation, sizeof(a.rotation)) == 0 &&
           a.corrections == b.corrections;
  }
};

// A GUID's text (8-4-4-4-12 hex digits, any case) in the Windows layout the game keeps it in;
// false for one that is not.
inline bool parse_template_guid(const std::string& text, std::array<std::uint8_t, 16>& guid) {
  if (text.size() != 36 || text[8] != '-' || text[13] != '-' || text[18] != '-' ||
      text[23] != '-') {
    return false;
  }
  std::string digits;
  for (char c : text) {
    if (c != '-') {
      digits.push_back(c);
    }
  }
  std::array<std::uint8_t, 16> bytes{};
  for (std::size_t i = 0; i < 16; ++i) {
    unsigned value = 0;
    for (std::size_t j = 0; j < 2; ++j) {
      const char c = digits[i * 2 + j];
      value <<= 4;
      if (c >= '0' && c <= '9') {
        value |= unsigned(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        value |= unsigned(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        value |= unsigned(c - 'A' + 10);
      } else {
        return false;
      }
    }
    bytes[i] = std::uint8_t(value);
  }
  // Data1 (4 bytes), Data2 and Data3 (2 each) are little-endian in memory; Data4 as written.
  guid = {bytes[3], bytes[2], bytes[1],  bytes[0],  bytes[5],  bytes[4],  bytes[7],  bytes[6],
          bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]};
  return true;
}

namespace smart_object_detail {

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

inline bool valid(const SmartObjectOrder& order) {
  if (order.id == 0) {
    return false;
  }
  if (order.remove) {
    return true;
  }
  for (float c : order.position) {
    if (!std::isfinite(c) || std::fabs(c) > kMaxSmartObjectCoordinate) {
      return false;
    }
  }
  for (float c : order.rotation) {
    if (!std::isfinite(c) || std::fabs(c) > 1.001f) {
      return false;
    }
  }
  if (order.corrections.size() > kMaxSmartObjectCorrections) {
    return false;
  }
  for (const SmartObjectCorrection& correction : order.corrections) {
    for (float c : correction.at) {
      if (!std::isfinite(c) || std::fabs(c) > kMaxCorrectionOffset) {
        return false;
      }
    }
    for (float c : correction.turn) {
      if (!std::isfinite(c) || std::fabs(c) > 1.001f) {
        return false;
      }
    }
  }
  return order.template_guid != std::array<std::uint8_t, 16>{};
}

}  // namespace smart_object_detail

// Empty for an order that cannot travel.
inline std::vector<std::uint8_t> encode_smart_object(const SmartObjectOrder& order) {
  using namespace smart_object_detail;
  if (!valid(order)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kSmartObjectTag, 8);
  w.write_bits(order.id, 32);
  w.write_bool(order.remove);
  if (order.remove) {
    return w.bytes();
  }
  for (const std::uint8_t byte : order.template_guid) {
    w.write_bits(byte, 8);
  }
  for (float c : order.position) {
    write_float(w, c);
  }
  for (float c : order.rotation) {
    write_float(w, c);
  }
  w.write_varuint(order.corrections.size());
  for (const SmartObjectCorrection& correction : order.corrections) {
    w.write_bits(correction.place, 16);
    for (float c : correction.at) {
      write_float(w, c);
    }
    for (float c : correction.turn) {
      write_float(w, c);
    }
  }
  return w.bytes();
}

inline bool decode_smart_object(const std::vector<std::uint8_t>& p, SmartObjectOrder& order) {
  using namespace smart_object_detail;
  if (p.empty() || p[0] != kSmartObjectTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  SmartObjectOrder decoded;
  decoded.id = std::uint32_t(r.read_bits(32));
  decoded.remove = r.read_bool();
  if (!decoded.remove) {
    for (std::uint8_t& byte : decoded.template_guid) {
      byte = std::uint8_t(r.read_bits(8));
    }
    for (float& c : decoded.position) {
      c = read_float(r);
    }
    for (float& c : decoded.rotation) {
      c = read_float(r);
    }
    const std::uint64_t count = r.read_varuint();
    if (r.overrun() || count > kMaxSmartObjectCorrections) {
      return false;
    }
    decoded.corrections.resize(std::size_t(count));
    for (SmartObjectCorrection& correction : decoded.corrections) {
      correction.place = std::uint16_t(r.read_bits(16));
      for (float& c : correction.at) {
        c = read_float(r);
      }
      for (float& c : correction.turn) {
        c = read_float(r);
      }
    }
  }
  if (r.overrun() || !valid(decoded)) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
