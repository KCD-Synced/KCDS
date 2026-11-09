#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"

// Buffs a resource defined itself (Buffs.register). The server keeps them, applies
// their stat changes and ends them; the player's own game shows each one in the status list as
// a buff of its own, a copy of the definition of the game buff whose icon it borrows, without
// that buff's effects.
//
//   CUSTOM_BUFF : server -> the player's own client, on the control channel (0):
//                 [tag][u8 kind][varuint name length][name]
//                 kind 1 (show) then: [16 bytes icon GUID][varuint label length][label]
//                                     [varint duration ms][varint remaining ms]
//                                     [varuint params length][params]
//                 params: the game's own buff modifiers in its buff table's text (buff_params,
//                 "mst*1.2,cap+50"), '' for none.
//                 Then, only for a buff with its own picture: [varuint resource length]
//                 [resource][varuint path length][path], the .dds that resource ships to
//                 clients, shown in place of the borrowed buff's icon.
//                 kind 2 (hide): nothing more.
//                 A show of a name the player has restarts it. A duration or remaining time
//                 below zero is a buff without end. The client drops every one when it leaves a
//                 world.
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kCustomBuffTag = 80;
// The bounds of Buffs.register (custom_buffs.cjs): a name, a label in bytes, the longest run.
inline constexpr std::size_t kCustomBuffNameMax = 64;
inline constexpr std::size_t kCustomBuffLabelMax = 128;
inline constexpr std::int64_t kCustomBuffDurationMaxMs = 7LL * 24 * 3600 * 1000;
inline constexpr std::size_t kCustomBuffParamsMax = 512;
// A picture's resource name and file path, each in bytes.
inline constexpr std::size_t kCustomBuffImageMax = 255;
// Orders a client holds before its game takes them.
inline constexpr std::size_t kMaxCustomBuffOrders = 256;

struct CustomBuff {
  enum class Kind : std::uint8_t {
    Show = 1,
    Hide = 2,
  };
  Kind kind = Kind::Show;
  std::string name;
  Guid icon{};                     // show: the game buff whose definition and icon it takes
  std::string label;               // show: the text of its entry
  std::int64_t duration_ms = -1;   // show: the full run, below zero none
  std::int64_t remaining_ms = -1;  // show: what is left of it, below zero none
  std::string params;              // show: its native modifiers (buff_params text), '' none
  std::string image_resource;      // show: the resource of its own picture, '' none
  std::string image_path;          // show: that picture's .dds in the resource, '' none

  friend bool operator==(const CustomBuff& a, const CustomBuff& b) {
    return a.kind == b.kind && a.name == b.name && a.icon == b.icon && a.label == b.label &&
           a.duration_ms == b.duration_ms && a.remaining_ms == b.remaining_ms &&
           a.params == b.params && a.image_resource == b.image_resource &&
           a.image_path == b.image_path;
  }
};

// A picture: both parts empty (none), or both 1 to kCustomBuffImageMax printable characters
// without spaces.
inline bool custom_buff_image_valid(const std::string& resource, const std::string& path) {
  if (resource.empty() != path.empty()) {
    return false;
  }
  for (const std::string* part : {&resource, &path}) {
    if (part->size() > kCustomBuffImageMax) {
      return false;
    }
    for (const char c : *part) {
      if (c < 0x21 || c > 0x7E) {
        return false;
      }
    }
  }
  return true;
}

// A name as Buffs.register takes it: 1 to kCustomBuffNameMax of A-Z a-z 0-9 : _ . -
inline bool custom_buff_name_valid(const std::string& name) {
  static constexpr char kAllowed[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789:_.-";
  return !name.empty() && name.size() <= kCustomBuffNameMax &&
         name.find_first_not_of(kAllowed) == std::string::npos;
}

// A label: 1 to kCustomBuffLabelMax bytes (UTF-8 text), none of them zero.
inline bool custom_buff_label_valid(const std::string& label) {
  return !label.empty() && label.size() <= kCustomBuffLabelMax &&
         label.find('\0') == std::string::npos;
}

inline bool custom_buff_time_valid(std::int64_t ms) {
  return ms <= kCustomBuffDurationMaxMs;
}

// Params: up to kCustomBuffParamsMax of letters, digits, _ . , + - * = % / and spaces.
inline bool custom_buff_params_valid(const std::string& params) {
  if (params.size() > kCustomBuffParamsMax) {
    return false;
  }
  for (const char c : params) {
    const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    const bool digit = c >= '0' && c <= '9';
    const bool sign = c == '_' || c == '.' || c == ',' || c == '+' || c == '-' || c == '*' ||
                      c == '=' || c == '%' || c == '/' || c == ' ';
    if (!letter && !digit && !sign) {
      return false;
    }
  }
  return true;
}

namespace detail {

inline void write_custom_buff_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(text.data()), text.size());
}

inline bool read_custom_buff_text(core::BitReader& r, std::size_t max, std::string& text) {
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || size > max) {
    return false;
  }
  text.resize(std::size_t(size));
  for (char& c : text) {
    c = char(r.read_bits(8));
  }
  return !r.overrun();
}

}  // namespace detail

// CUSTOM_BUFF; empty for an order that cannot travel.
inline std::vector<std::uint8_t> encode_custom_buff(const CustomBuff& buff) {
  using Kind = CustomBuff::Kind;
  if (!custom_buff_name_valid(buff.name)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kCustomBuffTag, 8);
  w.write_bits(std::uint8_t(buff.kind), 8);
  detail::write_custom_buff_text(w, buff.name);
  switch (buff.kind) {
    case Kind::Show:
      if (!custom_buff_label_valid(buff.label) || !custom_buff_time_valid(buff.duration_ms) ||
          !custom_buff_time_valid(buff.remaining_ms) || !custom_buff_params_valid(buff.params) ||
          !custom_buff_image_valid(buff.image_resource, buff.image_path)) {
        return {};
      }
      w.write_bytes(buff.icon.data(), buff.icon.size());
      detail::write_custom_buff_text(w, buff.label);
      w.write_varint(buff.duration_ms);
      w.write_varint(buff.remaining_ms);
      detail::write_custom_buff_text(w, buff.params);
      if (!buff.image_path.empty()) {
        detail::write_custom_buff_text(w, buff.image_resource);
        detail::write_custom_buff_text(w, buff.image_path);
      }
      return w.bytes();
    case Kind::Hide:
      return w.bytes();
  }
  return {};
}

inline bool decode_custom_buff(const std::vector<std::uint8_t>& p, CustomBuff& buff) {
  using Kind = CustomBuff::Kind;
  if (p.size() < 3 || p[0] != kCustomBuffTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  CustomBuff decoded;
  decoded.kind = Kind(r.read_bits(8));
  if (decoded.kind != Kind::Show && decoded.kind != Kind::Hide) {
    return false;
  }
  if (!detail::read_custom_buff_text(r, kCustomBuffNameMax, decoded.name) ||
      !custom_buff_name_valid(decoded.name)) {
    return false;
  }
  if (decoded.kind == Kind::Show) {
    for (std::uint8_t& byte : decoded.icon) {
      byte = std::uint8_t(r.read_bits(8));
    }
    if (!detail::read_custom_buff_text(r, kCustomBuffLabelMax, decoded.label) ||
        !custom_buff_label_valid(decoded.label)) {
      return false;
    }
    decoded.duration_ms = r.read_varint();
    decoded.remaining_ms = r.read_varint();
    if (!custom_buff_time_valid(decoded.duration_ms) ||
        !custom_buff_time_valid(decoded.remaining_ms)) {
      return false;
    }
    if (!detail::read_custom_buff_text(r, kCustomBuffParamsMax, decoded.params) ||
        !custom_buff_params_valid(decoded.params)) {
      return false;
    }
    // A buff with its own picture names it last.
    if (!r.overrun() && r.bits_remaining() != 0) {
      if (!detail::read_custom_buff_text(r, kCustomBuffImageMax, decoded.image_resource) ||
          !detail::read_custom_buff_text(r, kCustomBuffImageMax, decoded.image_path) ||
          decoded.image_path.empty() ||
          !custom_buff_image_valid(decoded.image_resource, decoded.image_path)) {
        return false;
      }
    }
  }
  if (r.overrun() || r.bits_remaining() != 0) {
    return false;
  }
  buff = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
