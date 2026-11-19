// Where a player's hands are held (Player.setHandTarget): the server keeps both hands in the
// NetworkPlayer field "handTargets", which every game reads for that player's body and solves its
// arms to (Client/runtime/animation/hand_targets.hpp), on top of whatever the body plays.
//
// The field: "" for none, else one entry per held hand, ';' between them, each
//   <hand>|<joint>|<x>,<y>,<z>|<rotation>|<pole>|<weight>|<blend>|<fp position>|<fp rotation>
// <hand> 'r' or 'l'; <joint> the joint the position is measured from ("" for the body's origin,
// its feet); positions in millimetres on the body's axes (x right, y forward, z up); rotations as
// four integers x,y,z,w in ten-thousandths (the hand's turn on the body's axes, "" to keep the
// turn the animation gives it); <pole> where the elbow points, in millimetres ("" for the
// animation's own bend); <weight> in hundredths (0 to 100); <blend> in milliseconds (0 to 10000);
// <fp ...> the position and rotation used instead in the owner's own first-person view ("" for
// none).
#pragma once

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kcd2s::integration {

inline constexpr std::size_t kMaxHandTargetsField = 512;
inline constexpr std::size_t kMaxHandJointName = 48;
// How far a hand target may be from where it is measured (metres).
inline constexpr float kMaxHandReach = 3.0f;
inline constexpr std::uint32_t kMaxHandBlendMs = 10000;

struct HandTarget {
  bool right = true;
  std::string joint;
  std::array<float, 3> position{};
  std::optional<std::array<float, 4>> rotation;
  std::optional<std::array<float, 3>> pole;
  float weight = 1.0f;
  float blend = 0.0f;
  std::optional<std::array<float, 3>> first_person_position;
  std::optional<std::array<float, 4>> first_person_rotation;

  friend bool operator==(const HandTarget&, const HandTarget&) = default;
};

namespace detail {

inline bool hand_number(std::string_view text, std::int64_t& value) {
  if (text.empty() || text.size() > 12) {
    return false;
  }
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc() && result.ptr == text.data() + text.size();
}

inline std::vector<std::string_view> hand_split(std::string_view text, char separator) {
  std::vector<std::string_view> parts;
  std::size_t start = 0;
  for (std::size_t at = 0; at <= text.size(); ++at) {
    if (at == text.size() || text[at] == separator) {
      parts.push_back(text.substr(start, at - start));
      start = at + 1;
    }
  }
  return parts;
}

template <std::size_t N>
bool hand_numbers(std::string_view text, double scale, std::array<float, N>& out) {
  const auto parts = hand_split(text, ',');
  if (parts.size() != N) {
    return false;
  }
  for (std::size_t index = 0; index < N; ++index) {
    std::int64_t value = 0;
    if (!hand_number(parts[index], value)) {
      return false;
    }
    out[index] = float(double(value) / scale);
  }
  return true;
}

template <std::size_t N>
void hand_append(std::string& field, const std::array<float, N>& values, double scale) {
  for (std::size_t index = 0; index < N; ++index) {
    if (index > 0) {
      field += ',';
    }
    field += std::to_string(std::llround(double(values[index]) * scale));
  }
}

inline bool hand_point(const std::array<float, 3>& point) {
  return std::isfinite(point[0]) && std::isfinite(point[1]) && std::isfinite(point[2]) &&
         std::sqrt(point[0] * point[0] + point[1] * point[1] + point[2] * point[2]) <=
             kMaxHandReach;
}

inline bool hand_turn(std::array<float, 4>& q) {
  const float size = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(size > 0.5f) || !(size < 1.5f)) {
    return false;
  }
  for (float& part : q) {
    part /= size;
  }
  return true;
}

inline bool hand_joint_name(std::string_view name) {
  if (name.size() > kMaxHandJointName) {
    return false;
  }
  for (const char c : name) {
    const bool letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !digit && c != '_' && c != ' ') {
      return false;
    }
  }
  return true;
}

}  // namespace detail

inline std::string hand_targets_field(const std::vector<HandTarget>& hands) {
  std::string field;
  for (const HandTarget& hand : hands) {
    if (!field.empty()) {
      field += ';';
    }
    field += hand.right ? 'r' : 'l';
    field += '|';
    field += hand.joint;
    field += '|';
    detail::hand_append(field, hand.position, 1000.0);
    field += '|';
    if (hand.rotation) {
      detail::hand_append(field, *hand.rotation, 10000.0);
    }
    field += '|';
    if (hand.pole) {
      detail::hand_append(field, *hand.pole, 1000.0);
    }
    field += '|';
    field += std::to_string(std::llround(double(hand.weight) * 100.0));
    field += '|';
    field += std::to_string(std::llround(double(hand.blend) * 1000.0));
    field += '|';
    if (hand.first_person_position) {
      detail::hand_append(field, *hand.first_person_position, 1000.0);
    }
    field += '|';
    if (hand.first_person_rotation) {
      detail::hand_append(field, *hand.first_person_rotation, 10000.0);
    }
  }
  return field;
}

// False for a field that is not one: malformed, too long, a hand twice, a position out of reach,
// a weight or blend out of range.
inline bool parse_hand_targets_field(const std::string& field, std::vector<HandTarget>& hands) {
  hands.clear();
  if (field.empty()) {
    return true;
  }
  if (field.size() > kMaxHandTargetsField) {
    return false;
  }
  std::vector<HandTarget> parsed;
  for (const std::string_view entry : detail::hand_split(field, ';')) {
    const auto parts = detail::hand_split(entry, '|');
    if (parts.size() != 9 || (parts[0] != "r" && parts[0] != "l")) {
      return false;
    }
    HandTarget hand;
    hand.right = parts[0] == "r";
    for (const HandTarget& other : parsed) {
      if (other.right == hand.right) {
        return false;
      }
    }
    if (!detail::hand_joint_name(parts[1])) {
      return false;
    }
    hand.joint = std::string(parts[1]);
    if (!detail::hand_numbers(parts[2], 1000.0, hand.position) ||
        !detail::hand_point(hand.position)) {
      return false;
    }
    if (!parts[3].empty()) {
      std::array<float, 4> q{};
      if (!detail::hand_numbers(parts[3], 10000.0, q) || !detail::hand_turn(q)) {
        return false;
      }
      hand.rotation = q;
    }
    if (!parts[4].empty()) {
      std::array<float, 3> pole{};
      if (!detail::hand_numbers(parts[4], 1000.0, pole) || !detail::hand_point(pole)) {
        return false;
      }
      hand.pole = pole;
    }
    std::int64_t weight = 0;
    std::int64_t blend = 0;
    if (!detail::hand_number(parts[5], weight) || weight < 0 || weight > 100 ||
        !detail::hand_number(parts[6], blend) || blend < 0 || blend > kMaxHandBlendMs) {
      return false;
    }
    hand.weight = float(weight) / 100.0f;
    hand.blend = float(blend) / 1000.0f;
    if (!parts[7].empty()) {
      std::array<float, 3> at{};
      if (!detail::hand_numbers(parts[7], 1000.0, at) || !detail::hand_point(at)) {
        return false;
      }
      hand.first_person_position = at;
    }
    if (!parts[8].empty()) {
      std::array<float, 4> q{};
      if (!detail::hand_numbers(parts[8], 10000.0, q) || !detail::hand_turn(q)) {
        return false;
      }
      hand.first_person_rotation = q;
    }
    parsed.push_back(std::move(hand));
  }
  hands = std::move(parsed);
  return true;
}

}  // namespace kcd2s::integration
