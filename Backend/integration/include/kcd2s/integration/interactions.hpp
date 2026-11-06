#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/math.hpp"

// Interactions a resource puts on things in the world (KCDS addition World.addInteraction, issue):
// the game's own "press [key] to ..." hint shows to a player who aims at a level entity
// (by its name) or a position within reach, and using it runs the resource's handler. What is
// aimed at is the entity's drawn box, or for a target without one (a helper entity, a position)
// an upright cylinder of target radius and target height standing on it.
//
//   INTERACTIONS      : server -> a player's client, on the control channel (0): the player's
//                       whole list, which replaces the one before:
//                         [tag][u8 n] then n x { [u32 id][u8 target 0 position | 1 entity |
//                         2 body][u32 body's network id, target 2 only]
//                         [f32 x3 position][u8 name length][name][f32 radius][u8 key]
//                         [u8 hold 0/1][f32 hold seconds][f32 target radius]
//                         [f32 target height][u16 text length][text] }
//                       little-endian; key is the index in kInteractionKeys, the text UTF-8.
//   INTERACTION_FIRED : the player's client -> server, on the control channel: [tag][u32 id], when
//                       the player used one (pressed, or held for its hold seconds). The server
//                       takes at most kMaxInteractionsFiredPerSecond a second from a client.
namespace kcd2s::integration {

inline constexpr std::uint8_t kInteractionsTag = 62;
inline constexpr std::uint8_t kInteractionFiredTag = 63;
// The most a client gets: all the u8 count holds.
inline constexpr std::size_t kMaxInteractions = 255;
inline constexpr std::size_t kInteractionNameMax = 255;
inline constexpr std::size_t kInteractionTextMax = 256;
inline constexpr float kInteractionRadiusMin = 0.5f;
inline constexpr float kInteractionRadiusMax = 10.0f;
inline constexpr float kInteractionHoldMin = 0.2f;
inline constexpr float kInteractionHoldMax = 10.0f;
inline constexpr float kInteractionTargetMin = 0.1f;
inline constexpr float kInteractionTargetRadiusMax = 5.0f;
inline constexpr float kInteractionTargetHeightMax = 10.0f;
inline constexpr unsigned kMaxInteractionsFiredPerSecond = 4;
// The game controls a hint can be on (the client's ActionHint keys); script_action is KCDS's own
// key (U), last so the indices of the others stay.
inline constexpr const char* kInteractionKeys[] = {
    "primary_action", "attack_special", "torch",        "companion_interaction",
    "distract",       "jump",           "script_action"};
inline constexpr std::size_t kInteractionKeyCount =
    sizeof(kInteractionKeys) / sizeof(kInteractionKeys[0]);

struct Interaction {
  std::uint32_t id{};
  // A level entity by its name, or a position when the name is empty.
  std::string entity;
  // A player's or an NPC's body by its network id, 0 for none: the target follows
  // the body. The name is then empty.
  std::uint32_t body = 0;
  core::Vec3 position{};
  float radius = 2;
  std::uint8_t key = 0;
  bool hold = false;
  float hold_seconds = 1;
  // The cylinder aimed at when the target draws no box.
  float target_radius = 0.6f;
  float target_height = 1.8f;
  std::string text;
};

inline bool valid_interaction(const Interaction& i) {
  const bool finite = std::isfinite(i.position.x) && std::isfinite(i.position.y) &&
                      std::isfinite(i.position.z) && std::isfinite(i.radius) &&
                      std::isfinite(i.hold_seconds) && std::isfinite(i.target_radius) &&
                      std::isfinite(i.target_height);
  const bool target =
      i.target_radius >= kInteractionTargetMin && i.target_radius <= kInteractionTargetRadiusMax &&
      i.target_height >= kInteractionTargetMin && i.target_height <= kInteractionTargetHeightMax;
  return i.id != 0 && finite && target && (i.body == 0 || i.entity.empty()) &&
         i.entity.size() <= kInteractionNameMax && i.text.size() <= kInteractionTextMax &&
         i.radius >= kInteractionRadiusMin && i.radius <= kInteractionRadiusMax &&
         i.key < kInteractionKeyCount && i.hold_seconds >= kInteractionHoldMin &&
         i.hold_seconds <= kInteractionHoldMax;
}

namespace interaction_detail {

inline void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(std::uint8_t(value >> shift));
  }
}

inline void put_f32(std::vector<std::uint8_t>& out, float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  put_u32(out, bits);
}

struct Reader {
  const std::vector<std::uint8_t>& in;
  std::size_t at = 0;
  bool ok = true;

  bool need(std::size_t n) {
    ok = ok && at + n <= in.size();
    return ok;
  }
  std::uint8_t u8() {
    return need(1) ? in[at++] : 0;
  }
  std::uint32_t u32() {
    if (!need(4)) {
      return 0;
    }
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
      value |= std::uint32_t(in[at + i]) << (8 * i);
    }
    at += 4;
    return value;
  }
  float f32() {
    const std::uint32_t bits = u32();
    float value = 0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
  }
  std::string text(std::size_t n) {
    if (!need(n)) {
      return {};
    }
    std::string value(in.begin() + std::ptrdiff_t(at), in.begin() + std::ptrdiff_t(at + n));
    at += n;
    return value;
  }
};

}  // namespace interaction_detail

// Empty when the list cannot travel (too many, or one is not valid_interaction).
inline std::vector<std::uint8_t> encode_interactions(const std::vector<Interaction>& list) {
  if (list.size() > kMaxInteractions) {
    return {};
  }
  std::vector<std::uint8_t> out = {kInteractionsTag, std::uint8_t(list.size())};
  for (const Interaction& i : list) {
    if (!valid_interaction(i)) {
      return {};
    }
    interaction_detail::put_u32(out, i.id);
    out.push_back(i.body != 0 ? 2 : i.entity.empty() ? 0 : 1);
    if (i.body != 0) {
      interaction_detail::put_u32(out, i.body);
    }
    interaction_detail::put_f32(out, i.position.x);
    interaction_detail::put_f32(out, i.position.y);
    interaction_detail::put_f32(out, i.position.z);
    out.push_back(std::uint8_t(i.entity.size()));
    out.insert(out.end(), i.entity.begin(), i.entity.end());
    interaction_detail::put_f32(out, i.radius);
    out.push_back(i.key);
    out.push_back(i.hold ? 1 : 0);
    interaction_detail::put_f32(out, i.hold_seconds);
    interaction_detail::put_f32(out, i.target_radius);
    interaction_detail::put_f32(out, i.target_height);
    out.push_back(std::uint8_t(i.text.size()));
    out.push_back(std::uint8_t(i.text.size() >> 8));
    out.insert(out.end(), i.text.begin(), i.text.end());
  }
  return out;
}

inline bool decode_interactions(const std::vector<std::uint8_t>& in,
                                std::vector<Interaction>& list) {
  interaction_detail::Reader r{in};
  if (r.u8() != kInteractionsTag) {
    return false;
  }
  const std::size_t n = r.u8();
  if (n > kMaxInteractions) {
    return false;
  }
  std::vector<Interaction> decoded;
  for (std::size_t k = 0; k < n && r.ok; ++k) {
    Interaction i;
    i.id = r.u32();
    const std::uint8_t target = r.u8();
    if (target == 2) {
      i.body = r.u32();
    }
    i.position = {r.f32(), r.f32(), r.f32()};
    i.entity = r.text(r.u8());
    i.radius = r.f32();
    i.key = r.u8();
    const std::uint8_t hold = r.u8();
    i.hold = hold == 1;
    i.hold_seconds = r.f32();
    i.target_radius = r.f32();
    i.target_height = r.f32();
    const std::size_t text_low = r.u8();
    const std::size_t text_high = r.u8();
    i.text = r.text(text_low | (text_high << 8));
    if (!r.ok || target > 2 || hold > 1 || (target == 1) == i.entity.empty() ||
        (target == 2) != (i.body != 0) || !valid_interaction(i)) {
      return false;
    }
    decoded.push_back(std::move(i));
  }
  if (!r.ok || r.at != in.size()) {
    return false;
  }
  list = std::move(decoded);
  return true;
}

inline std::vector<std::uint8_t> encode_interaction_fired(std::uint32_t id) {
  std::vector<std::uint8_t> out = {kInteractionFiredTag};
  interaction_detail::put_u32(out, id);
  return out;
}

inline bool decode_interaction_fired(const std::vector<std::uint8_t>& in, std::uint32_t& id) {
  interaction_detail::Reader r{in};
  if (in.size() != 5 || r.u8() != kInteractionFiredTag) {
    return false;
  }
  id = r.u32();
  return id != 0;
}

}  // namespace kcd2s::integration
