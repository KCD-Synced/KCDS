#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/integration/hands.hpp"

// A player's appearance: four of the game's character-component names and the gender
// whose catalog they come from.
//   APPEARANCE : the parts a resource asked the player to change (server -> its owner's client,
//                control channel 0). The owner puts them on and publishes what it wears in the
//                NetworkPlayer field "appearance" (appearance_field), which the server reports to
//                the resources and every other client puts on the player's replica.
// The owner also publishes the five components its body is built from (body, head, hair, beard,
// underwear; its own base parts where nothing was chosen) in the field "bodyParts", which every
// other client puts on the replica: a replica's soul gets other base parts of its own.
//   BODY_VISIBLE : whether a resource wants the player's body drawn (server -> its owner's client).
//                The owner stops or starts drawing it and publishes that in the NetworkPlayer field
//                "bodyVisible".
//   DISGUISE   : the prop-catalog mesh a resource wants the player's body drawn as, by its path,
//                or "" for its own body (server -> its owner's client). The owner publishes the
//                mesh it actually wears in the NetworkPlayer field "disguise".
namespace kcd2s::integration {
inline constexpr std::uint8_t kAppearanceTag = 11;
inline constexpr std::uint8_t kBodyVisibleTag = 12;
inline constexpr std::uint8_t kDisguiseTag = 13;
// WORN: server -> the player's own client: [tag][u8 n] then n x [16 bytes item class]:
// the items the player wears for a while outside their inventory (the whole list, which replaces
// the one before). The client makes and puts on what is new and takes away what is gone; everyone
// else sees them through the player's outfit.
inline constexpr std::uint8_t kWornTag = 163;
inline constexpr std::size_t kMaxWorn = 8;
// Longer than any mesh path of the game's catalog.
inline constexpr std::size_t kDisguiseMax = 255;
// Longer than any of the game's character-component names.
inline constexpr std::size_t kAppearanceNameMax = 64;

// What a body looks like. An empty part is the game's own (the default body).
struct Appearance {
  std::string gender = "male";
  std::string body;
  std::string head;
  std::string hair;
  std::string beard;
  friend bool operator==(const Appearance& a, const Appearance& b) {
    return a.gender == b.gender && a.body == b.body && a.head == b.head && a.hair == b.hair &&
           a.beard == b.beard;
  }
  friend bool operator!=(const Appearance& a, const Appearance& b) {
    return !(a == b);
  }
};

// The parts a request names; a named part with an empty name goes back to the game's own.
struct AppearanceRequest {
  std::optional<std::string> gender;
  std::optional<std::string> body;
  std::optional<std::string> head;
  std::optional<std::string> hair;
  std::optional<std::string> beard;
};

namespace detail {
// A component name or gender: printable ASCII without the field separator.
inline bool appearance_name(const std::string& name) {
  if (name.size() > kAppearanceNameMax) {
    return false;
  }
  for (char c : name) {
    if (c < 0x21 || c > 0x7e || c == '|') {
      return false;
    }
  }
  return true;
}

inline void write_part(std::vector<std::uint8_t>& p, const std::optional<std::string>& part) {
  if (!part) {
    p.push_back(0);
    return;
  }
  p.push_back(1);
  p.push_back(static_cast<std::uint8_t>(part->size()));
  p.insert(p.end(), part->begin(), part->end());
}

inline bool read_part(const std::vector<std::uint8_t>& p, std::size_t& at,
                      std::optional<std::string>& part) {
  if (at >= p.size()) {
    return false;
  }
  if (p[at] == 0) {
    ++at;
    part.reset();
    return true;
  }
  if (p[at] != 1 || at + 1 >= p.size()) {
    return false;
  }
  const std::size_t size = p[at + 1];
  at += 2;
  if (size > kAppearanceNameMax || at + size > p.size()) {
    return false;
  }
  part = std::string(p.begin() + std::ptrdiff_t(at), p.begin() + std::ptrdiff_t(at + size));
  at += size;
  return appearance_name(*part);
}
}  // namespace detail

// `count` component names (detail::appearance_name, an empty one allowed) between count - 1 '|':
// the form of an NPC's appearance and a horse's look.
inline bool valid_component_names(const std::string& text, std::size_t count) {
  std::size_t parts = 1;
  std::string name;
  for (const char c : text) {
    if (c != '|') {
      name.push_back(c);
      continue;
    }
    if (!detail::appearance_name(name)) {
      return false;
    }
    name.clear();
    ++parts;
  }
  return parts == count && detail::appearance_name(name);
}

// False for a name that cannot travel (too long, or not a component name at all).
inline bool valid_appearance_request(const AppearanceRequest& request) {
  for (const auto* part :
       {&request.gender, &request.body, &request.head, &request.hair, &request.beard}) {
    if (*part && !detail::appearance_name(**part)) {
      return false;
    }
  }
  return true;
}

inline std::vector<std::uint8_t> encode_appearance(const AppearanceRequest& request) {
  std::vector<std::uint8_t> p{kAppearanceTag};
  detail::write_part(p, request.gender);
  detail::write_part(p, request.body);
  detail::write_part(p, request.head);
  detail::write_part(p, request.hair);
  detail::write_part(p, request.beard);
  return p;
}

inline bool decode_appearance(const std::vector<std::uint8_t>& p, AppearanceRequest& request) {
  if (p.empty() || p[0] != kAppearanceTag) {
    return false;
  }
  std::size_t at = 1;
  AppearanceRequest decoded;
  for (auto* part :
       {&decoded.gender, &decoded.body, &decoded.head, &decoded.hair, &decoded.beard}) {
    if (!detail::read_part(p, at, *part)) {
      return false;
    }
  }
  if (at != p.size()) {
    return false;
  }
  request = std::move(decoded);
  return true;
}

inline std::vector<std::uint8_t> encode_body_visible(bool visible) {
  return {kBodyVisibleTag, std::uint8_t(visible ? 1 : 0)};
}

inline bool decode_body_visible(const std::vector<std::uint8_t>& p, bool& visible) {
  if (p.size() != 2 || p[0] != kBodyVisibleTag || p[1] > 1) {
    return false;
  }
  visible = p[1] == 1;
  return true;
}

// A mesh path: printable ASCII, no longer than kDisguiseMax; "" is the player's own body.
inline bool valid_disguise(const std::string& model) {
  if (model.size() > kDisguiseMax) {
    return false;
  }
  for (char c : model) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

inline std::vector<std::uint8_t> encode_disguise(const std::string& model) {
  std::vector<std::uint8_t> p{kDisguiseTag, std::uint8_t(model.size())};
  p.insert(p.end(), model.begin(), model.end());
  return p;
}

inline std::vector<std::uint8_t> encode_worn(const std::vector<ItemClassId>& items) {
  if (items.size() > kMaxWorn) {
    return {};
  }
  std::vector<std::uint8_t> p{kWornTag, std::uint8_t(items.size())};
  for (const ItemClassId& item : items) {
    p.insert(p.end(), item.begin(), item.end());
  }
  return p;
}

inline bool decode_worn(const std::vector<std::uint8_t>& p, std::vector<ItemClassId>& items) {
  if (p.size() < 2 || p[0] != kWornTag || p[1] > kMaxWorn ||
      p.size() != std::size_t(2 + 16 * p[1])) {
    return false;
  }
  std::vector<ItemClassId> decoded(p[1]);
  for (std::size_t i = 0; i < decoded.size(); ++i) {
    std::copy(p.begin() + std::ptrdiff_t(2 + 16 * i), p.begin() + std::ptrdiff_t(2 + 16 * (i + 1)),
              decoded[i].begin());
  }
  items = std::move(decoded);
  return true;
}

inline bool decode_disguise(const std::vector<std::uint8_t>& p, std::string& model) {
  if (p.size() < 2 || p[0] != kDisguiseTag || p.size() != std::size_t(2 + p[1])) {
    return false;
  }
  std::string decoded(p.begin() + 2, p.end());
  if (!valid_disguise(decoded)) {
    return false;
  }
  model = std::move(decoded);
  return true;
}

// The five components a body is built from, in the game's order (body, head, hair, beard,
// underwear), and the gender of the tree they come from.
inline constexpr std::size_t kBodyParts = 6;
using BodyParts = std::vector<std::string>;

// The "bodyParts" field: the five names and the gender separated by '|'.
inline std::string body_parts_field(const BodyParts& parts) {
  std::string field;
  for (std::size_t index = 0; index < parts.size(); ++index) {
    if (index) {
      field += '|';
    }
    field += parts[index];
  }
  return field;
}

// False for a field that is not five component names and a gender (not reported yet: "").
inline bool parse_body_parts_field(const std::string& field, BodyParts& parts) {
  BodyParts parsed(1);
  for (char c : field) {
    if (c == '|') {
      parsed.emplace_back();
    } else {
      parsed.back().push_back(c);
    }
  }
  if (parsed.size() != kBodyParts) {
    return false;
  }
  for (const std::string& name : parsed) {
    if (!detail::appearance_name(name)) {
      return false;
    }
  }
  if (parsed.back() != "male" && parsed.back() != "female") {
    return false;
  }
  parts = std::move(parsed);
  return true;
}

// The "appearance" field: "gender|body|head|hair|beard".
inline std::string appearance_field(const Appearance& appearance) {
  return appearance.gender + '|' + appearance.body + '|' + appearance.head + '|' + appearance.hair +
         '|' + appearance.beard;
}

// False for a field that is not an appearance (an owner that has not reported one yet sends
// "").
inline bool parse_appearance_field(const std::string& field, Appearance& appearance) {
  std::vector<std::string> parts(1);
  for (char c : field) {
    if (c == '|') {
      parts.emplace_back();
    } else {
      parts.back().push_back(c);
    }
  }
  if (parts.size() != 5) {
    return false;
  }
  for (const std::string& part : parts) {
    if (!detail::appearance_name(part)) {
      return false;
    }
  }
  if (parts[0] != "male" && parts[0] != "female") {
    return false;
  }
  appearance = Appearance{parts[0], parts[1], parts[2], parts[3], parts[4]};
  return true;
}
}  // namespace kcd2s::integration
