#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// World markers and blips: what a script shows a player in the world
// (a marker at a place) and on the compass and map (a blip). The server sends the show once, the
// hide once, and the show again when the look changes; ids are the script's entity ids.
//
//   MARKER_SHOW : server -> a ready player's client, control channel (0):
//                 [tag][varuint marker][material][shape][3 f32 position][4 f32 rotation]
//                 [f32 size][f32 height][f32 spin][f32 bob][u32 color]
//                 material and shape are [varuint length][bytes], printable ASCII, no spaces.
//   MARKER_HIDE : [tag][varuint marker]
//   BLIP_SHOW   : [tag][varuint blip][type][varuint markState][label][u8 flags][3 f32 position]
//                 [image resource][image path]
//                 type is the icon name (ASCII text as above); label is [varuint length][UTF-8
//                 bytes]; flags: 1 compass, 2 map; the image (ASCII text, both empty for none) is
//                 the blip's own icon, a .dds its resource ships to the clients.
//   BLIP_HIDE   : [tag][varuint blip]
//
// Client/runtime/markers/ draws them.
namespace kcd2s::integration {

inline constexpr std::uint8_t kMarkerShowTag = 90;
inline constexpr std::uint8_t kMarkerHideTag = 91;
inline constexpr std::uint8_t kBlipShowTag = 92;
inline constexpr std::uint8_t kBlipHideTag = 93;

inline constexpr std::size_t kMarkerMaterialMax = 255;
inline constexpr std::size_t kMarkerShapeMax = 32;
inline constexpr std::size_t kBlipTypeMax = 64;
inline constexpr std::size_t kBlipLabelMax = 128;
inline constexpr float kMarkerCoordinateMax = 100000.0f;
inline constexpr float kMarkerSizeMax = 1000.0f;
// What a client keeps queued before it applies the orders (a frame's worth is a handful).
inline constexpr std::size_t kMaxMarkerOrders = 256;

inline constexpr std::uint8_t kBlipCompassBit = 1;
inline constexpr std::uint8_t kBlipMapBit = 2;

struct MarkerShow {
  std::uint32_t marker = 0;
  std::string material;
  std::string shape;
  float position[3] = {0, 0, 0};
  float rotation[4] = {0, 0, 0, 1};
  float size = 1.0f;
  float height = 0.0f;
  float spin = 0.0f;
  float bob = 0.0f;
  std::uint32_t color = 0;  // packed 0xRRGGBBAA
};

struct BlipShow {
  std::uint32_t blip = 0;
  std::string type;
  std::uint32_t markState = 0;
  std::string label;
  bool compass = true;
  bool map = true;
  // Whether the map legend's switch for its type hides it (the client Blip's `legend`; a server
  // blip always follows it, so the wire leaves it out).
  bool legend = true;
  float position[3] = {0, 0, 0};
  // KCDS: the blip's own icon, a .dds its resource ships to the clients (both empty for none).
  std::string image_resource;
  std::string image_path;
};

// A show or a hide of one marker, in the order the server sent them.
struct MarkerOrder {
  bool show = false;
  MarkerShow marker;  // only `marker.marker` is set for a hide
};

struct BlipOrder {
  bool show = false;
  BlipShow blip;  // only `blip.blip` is set for a hide
};

namespace marker_detail {

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

// Printable ASCII without spaces, at most `max` bytes.
inline bool name_valid(const std::string& text, std::size_t max) {
  std::size_t printable = 0;
  while (printable < text.size() && text[printable] >= 0x21 && text[printable] <= 0x7E) {
    ++printable;
  }
  return printable == text.size() && text.size() <= max;
}

// UTF-8 text without control characters, at most `max` bytes.
inline bool label_valid(const std::string& text, std::size_t max) {
  std::size_t clean = 0;
  while (clean < text.size() && std::uint8_t(text[clean]) >= 0x20 && text[clean] != 0x7F) {
    ++clean;
  }
  return clean == text.size() && text.size() <= max;
}

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  for (const char c : text) {
    w.write_bits(std::uint8_t(c), 8);
  }
}

inline bool read_text(core::BitReader& r, std::string& text, std::size_t max) {
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || size > max || size * 8 > r.bits_remaining()) {
    return false;
  }
  text.resize(std::size_t(size));
  for (char& c : text) {
    c = char(r.read_bits(8));
  }
  return !r.overrun();
}

inline bool coordinates_valid(const float* values, std::size_t count) {
  for (std::size_t i = 0; i < count; ++i) {
    if (!std::isfinite(values[i]) || std::fabs(values[i]) > kMarkerCoordinateMax) {
      return false;
    }
  }
  return true;
}

inline bool valid(const MarkerShow& m) {
  if (!name_valid(m.material, kMarkerMaterialMax) || m.material.empty() ||
      !name_valid(m.shape, kMarkerShapeMax) || !coordinates_valid(m.position, 3) ||
      !coordinates_valid(m.rotation, 4) || !std::isfinite(m.size) || m.size < 0.0f ||
      m.size > kMarkerSizeMax || !std::isfinite(m.height) || !std::isfinite(m.spin) ||
      !std::isfinite(m.bob)) {
    return false;
  }
  const double length_squared =
      double(m.rotation[0]) * m.rotation[0] + double(m.rotation[1]) * m.rotation[1] +
      double(m.rotation[2]) * m.rotation[2] + double(m.rotation[3]) * m.rotation[3];
  return std::fabs(length_squared - 1.0) <= 0.01;
}

inline bool valid(const BlipShow& b) {
  const bool image_ok = b.image_resource.empty() == b.image_path.empty() &&
                        name_valid(b.image_resource, kMarkerMaterialMax) &&
                        name_valid(b.image_path, kMarkerMaterialMax);
  return name_valid(b.type, kBlipTypeMax) && !b.type.empty() &&
         label_valid(b.label, kBlipLabelMax) && coordinates_valid(b.position, 3) && image_ok;
}

inline std::vector<std::uint8_t> encode_id(std::uint8_t tag, std::uint32_t id) {
  core::BitWriter w;
  w.write_bits(tag, 8);
  w.write_varuint(id);
  return w.bytes();
}

inline bool decode_id(std::uint8_t tag, const std::vector<std::uint8_t>& p, std::uint32_t& id) {
  if (p.empty() || p[0] != tag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value > 0xffffffffu) {
    return false;
  }
  id = std::uint32_t(value);
  return true;
}

}  // namespace marker_detail

// Empty for a marker that cannot travel (bad material, shape, numbers or rotation).
inline std::vector<std::uint8_t> encode_marker_show(const MarkerShow& m) {
  if (!marker_detail::valid(m)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kMarkerShowTag, 8);
  w.write_varuint(m.marker);
  marker_detail::write_text(w, m.material);
  marker_detail::write_text(w, m.shape);
  for (const float value : m.position) {
    marker_detail::write_float(w, value);
  }
  for (const float value : m.rotation) {
    marker_detail::write_float(w, value);
  }
  marker_detail::write_float(w, m.size);
  marker_detail::write_float(w, m.height);
  marker_detail::write_float(w, m.spin);
  marker_detail::write_float(w, m.bob);
  w.write_bits(m.color, 32);
  return w.bytes();
}

inline bool decode_marker_show(const std::vector<std::uint8_t>& p, MarkerShow& out) {
  if (p.empty() || p[0] != kMarkerShowTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  MarkerShow m;
  const std::uint64_t id = r.read_varuint();
  if (r.overrun() || id > 0xffffffffu) {
    return false;
  }
  m.marker = std::uint32_t(id);
  if (!marker_detail::read_text(r, m.material, kMarkerMaterialMax) ||
      !marker_detail::read_text(r, m.shape, kMarkerShapeMax)) {
    return false;
  }
  for (float& value : m.position) {
    value = marker_detail::read_float(r);
  }
  for (float& value : m.rotation) {
    value = marker_detail::read_float(r);
  }
  m.size = marker_detail::read_float(r);
  m.height = marker_detail::read_float(r);
  m.spin = marker_detail::read_float(r);
  m.bob = marker_detail::read_float(r);
  m.color = std::uint32_t(r.read_bits(32));
  if (r.overrun() || !marker_detail::valid(m)) {
    return false;
  }
  out = std::move(m);
  return true;
}

inline std::vector<std::uint8_t> encode_marker_hide(std::uint32_t marker) {
  return marker_detail::encode_id(kMarkerHideTag, marker);
}

inline bool decode_marker_hide(const std::vector<std::uint8_t>& p, std::uint32_t& marker) {
  return marker_detail::decode_id(kMarkerHideTag, p, marker);
}

// Empty for a blip that cannot travel (bad icon name, label or position).
inline std::vector<std::uint8_t> encode_blip_show(const BlipShow& b) {
  if (!marker_detail::valid(b)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kBlipShowTag, 8);
  w.write_varuint(b.blip);
  marker_detail::write_text(w, b.type);
  w.write_varuint(b.markState);
  marker_detail::write_text(w, b.label);
  w.write_bits(std::uint8_t((b.compass ? kBlipCompassBit : 0) | (b.map ? kBlipMapBit : 0)), 8);
  for (const float value : b.position) {
    marker_detail::write_float(w, value);
  }
  marker_detail::write_text(w, b.image_resource);
  marker_detail::write_text(w, b.image_path);
  return w.bytes();
}

inline bool decode_blip_show(const std::vector<std::uint8_t>& p, BlipShow& out) {
  if (p.empty() || p[0] != kBlipShowTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  BlipShow b;
  const std::uint64_t id = r.read_varuint();
  if (r.overrun() || id > 0xffffffffu) {
    return false;
  }
  b.blip = std::uint32_t(id);
  if (!marker_detail::read_text(r, b.type, kBlipTypeMax)) {
    return false;
  }
  const std::uint64_t state = r.read_varuint();
  if (r.overrun() || state > 0xffffffffu) {
    return false;
  }
  b.markState = std::uint32_t(state);
  if (!marker_detail::read_text(r, b.label, kBlipLabelMax)) {
    return false;
  }
  const std::uint8_t flags = std::uint8_t(r.read_bits(8));
  b.compass = (flags & kBlipCompassBit) != 0;
  b.map = (flags & kBlipMapBit) != 0;
  for (float& value : b.position) {
    value = marker_detail::read_float(r);
  }
  if (!marker_detail::read_text(r, b.image_resource, kMarkerMaterialMax) ||
      !marker_detail::read_text(r, b.image_path, kMarkerMaterialMax)) {
    return false;
  }
  if (r.overrun() || !marker_detail::valid(b)) {
    return false;
  }
  out = std::move(b);
  return true;
}

inline std::vector<std::uint8_t> encode_blip_hide(std::uint32_t blip) {
  return marker_detail::encode_id(kBlipHideTag, blip);
}

inline bool decode_blip_hide(const std::vector<std::uint8_t>& p, std::uint32_t& blip) {
  return marker_detail::decode_id(kBlipHideTag, p, blip);
}

}  // namespace kcd2s::integration
