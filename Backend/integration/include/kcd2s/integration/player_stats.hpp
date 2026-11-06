#pragma once
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// A player's stats as its own client reads them off its body: what Player.getStat,
// derivedStat and scaledStats return on the server and on every client. The owner publishes
// them in two NetworkPlayer fields:
//   "vitals": the four writable states (health, stamina, exhaust, hunger) and the five conditions
//             (bleeding, sleeping, consciousness, drunkenness, poisoning), which follow the body
//             as it changes (vitals_field);
//   "stats":  the twenty other derived stats, sampled every 250 ms, then the three attributes as
//             the engine's relative values (stats_field);
//   "abilities": what the body can hold and do, sampled every 250 ms
//             (abilities_field): the four states' maxima and the stamina its injuries allow, the
//             main level, the three attributes and the nine combat and survival skills as the
//             game evaluates them (with every modifier), then the nine skills as the engine's
//             relative values.
// A field is a list of integers separated by ',': the vitals and derived stats in hundredths of a
// native unit, the relative attributes in ten-thousandths. "" until the owner reports.
//
// SET_STAT (control channel 0, server -> the player's own client): the server's Player.setStat
// and fillStat, a writable state and its new value; tags 1-14 are taken by control.hpp,
// world_config.hpp, knockout.hpp, appearance.hpp and player_identity.hpp.
// ADD_STAT (the same channel and direction): the server's Player.addStat, a writable
// state and a change the owner's game adds to its current value, within 0 and the state's maximum.
namespace kcd2s::integration {

inline constexpr std::array<const char*, 9> kVitalNames = {
    "health",   "stamina",       "exhaust",     "hunger",   "bleeding",
    "sleeping", "consciousness", "drunkenness", "poisoning"};
// The first kWritableStateCount vitals are the writable states, in the soul's own order.
inline constexpr std::size_t kWritableStateCount = 4;
inline constexpr std::array<const char*, 20> kDerivedNames = {"charisma",
                                                              "visibility",
                                                              "conspicuousness",
                                                              "noise",
                                                              "dirtiness",
                                                              "bloodiness",
                                                              "smell",
                                                              "smellIntensity",
                                                              "fragrance",
                                                              "carriedWeight",
                                                              "inventoryCapacity",
                                                              "encumbrance",
                                                              "hangover",
                                                              "alcoholism",
                                                              "armorRating",
                                                              "overallArmorDefense",
                                                              "overallWeaponAttack",
                                                              "normalizedRunSpeed",
                                                              "runSpeedBase",
                                                              "morale"};
inline constexpr std::array<const char*, 3> kRelativeNames = {"strength", "agility", "vitality"};

inline constexpr std::array<const char*, 5> kMaximumNames = {
    "maxHealth", "maxStamina", "staminaCap", "maxExhaust", "maxHunger"};
inline constexpr std::array<const char*, 3> kAttributeNames = {"strength", "agility", "vitality"};
// SoulSkills, in the API's order.
inline constexpr std::array<const char*, 9> kSkillNames = {
    "fencing",      "survival", "defense",      "sword",  "heavyWeapons",
    "marksmanship", "dagger",   "largeWeapons", "unarmed"};

// Units per value in the fields.
inline constexpr double kStatUnits = 100.0;
inline constexpr double kRelativeUnits = 10000.0;
// A value beyond this many units is cut to it (no native stat comes near).
inline constexpr std::int64_t kMaxFieldUnits = 1000000000000;

struct PlayerVitals {
  std::array<float, kVitalNames.size()> values{};

  friend bool operator==(const PlayerVitals& a, const PlayerVitals& b) {
    return a.values == b.values;
  }
};

struct PlayerStatsSample {
  std::array<float, kDerivedNames.size()> derived{};
  std::array<float, kRelativeNames.size()> relative{};

  friend bool operator==(const PlayerStatsSample& a, const PlayerStatsSample& b) {
    return a.derived == b.derived && a.relative == b.relative;
  }
};

struct PlayerAbilities {
  std::array<float, kMaximumNames.size()> maxima{};
  float level = 0;
  std::array<float, kAttributeNames.size()> attributes{};
  std::array<float, kSkillNames.size()> skills{};
  std::array<float, kSkillNames.size()> relative_skills{};

  friend bool operator==(const PlayerAbilities& a, const PlayerAbilities& b) {
    return a.maxima == b.maxima && a.level == b.level && a.attributes == b.attributes &&
           a.skills == b.skills && a.relative_skills == b.relative_skills;
  }
};

namespace detail {

inline std::int64_t field_units(float value, double units) {
  if (!std::isfinite(value)) {
    return 0;
  }
  const double scaled = std::round(double(value) * units);
  if (scaled > double(kMaxFieldUnits)) {
    return kMaxFieldUnits;
  }
  if (scaled < -double(kMaxFieldUnits)) {
    return -kMaxFieldUnits;
  }
  return std::int64_t(scaled);
}

inline void append_units(std::string& field, float value, double units) {
  if (!field.empty()) {
    field += ',';
  }
  field += std::to_string(field_units(value, units));
}

// Reads the next integer of a field at `at` (then past its ','); false for anything else.
inline bool next_units(const std::string& field, std::size_t& at, double units, float& value) {
  std::size_t end = field.find(',', at);
  if (end == std::string::npos) {
    end = field.size();
  }
  std::size_t digit = at;
  const bool negative = digit < end && field[digit] == '-';
  if (negative) {
    ++digit;
  }
  if (digit == end || end - digit > 13) {
    return false;
  }
  std::int64_t magnitude = 0;
  for (; digit < end; ++digit) {
    if (field[digit] < '0' || field[digit] > '9') {
      return false;
    }
    magnitude = magnitude * 10 + (field[digit] - '0');
  }
  if (magnitude > kMaxFieldUnits) {
    return false;
  }
  value = float(double(negative ? -magnitude : magnitude) / units);
  at = end + 1;
  return true;
}

}  // namespace detail

// The values as the fields carry them: rounded to a hundredth (relative values to a
// ten-thousandth), anything not finite 0.
inline float stat_field_value(float value) {
  return float(double(detail::field_units(value, kStatUnits)) / kStatUnits);
}
inline float relative_field_value(float value) {
  return float(double(detail::field_units(value, kRelativeUnits)) / kRelativeUnits);
}
// The same as the decimal number scripts read (87.26, not the float's 87.26000213623047).
inline double stat_number(float value) {
  return double(detail::field_units(value, kStatUnits)) / kStatUnits;
}
inline double relative_number(float value) {
  return double(detail::field_units(value, kRelativeUnits)) / kRelativeUnits;
}

inline std::string vitals_field(const PlayerVitals& vitals) {
  std::string field;
  for (float value : vitals.values) {
    detail::append_units(field, value, kStatUnits);
  }
  return field;
}

inline bool parse_vitals_field(const std::string& field, PlayerVitals& vitals) {
  PlayerVitals parsed;
  std::size_t at = 0;
  for (float& value : parsed.values) {
    if (at > field.size() || !detail::next_units(field, at, kStatUnits, value)) {
      return false;
    }
  }
  if (at != field.size() + 1) {
    return false;
  }
  vitals = parsed;
  return true;
}

inline std::string stats_field(const PlayerStatsSample& sample) {
  std::string field;
  for (float value : sample.derived) {
    detail::append_units(field, value, kStatUnits);
  }
  for (float value : sample.relative) {
    detail::append_units(field, value, kRelativeUnits);
  }
  return field;
}

inline bool parse_stats_field(const std::string& field, PlayerStatsSample& sample) {
  PlayerStatsSample parsed;
  std::size_t at = 0;
  for (float& value : parsed.derived) {
    if (at > field.size() || !detail::next_units(field, at, kStatUnits, value)) {
      return false;
    }
  }
  for (float& value : parsed.relative) {
    if (at > field.size() || !detail::next_units(field, at, kRelativeUnits, value)) {
      return false;
    }
  }
  if (at != field.size() + 1) {
    return false;
  }
  sample = parsed;
  return true;
}

inline std::string abilities_field(const PlayerAbilities& abilities) {
  std::string field;
  for (float value : abilities.maxima) {
    detail::append_units(field, value, kStatUnits);
  }
  detail::append_units(field, abilities.level, kStatUnits);
  for (float value : abilities.attributes) {
    detail::append_units(field, value, kStatUnits);
  }
  for (float value : abilities.skills) {
    detail::append_units(field, value, kStatUnits);
  }
  for (float value : abilities.relative_skills) {
    detail::append_units(field, value, kRelativeUnits);
  }
  return field;
}

inline bool parse_abilities_field(const std::string& field, PlayerAbilities& abilities) {
  PlayerAbilities parsed;
  std::size_t at = 0;
  const auto next = [&](float& value, double units) {
    return at <= field.size() && detail::next_units(field, at, units, value);
  };
  for (float& value : parsed.maxima) {
    if (!next(value, kStatUnits)) {
      return false;
    }
  }
  if (!next(parsed.level, kStatUnits)) {
    return false;
  }
  for (float& value : parsed.attributes) {
    if (!next(value, kStatUnits)) {
      return false;
    }
  }
  for (float& value : parsed.skills) {
    if (!next(value, kStatUnits)) {
      return false;
    }
  }
  for (float& value : parsed.relative_skills) {
    if (!next(value, kRelativeUnits)) {
      return false;
    }
  }
  if (at != field.size() + 1) {
    return false;
  }
  abilities = parsed;
  return true;
}

// SET_STAT: the state (0 health, 1 stamina, 2 exhaust, 3 hunger) and the value, a float in
// little-endian byte order. The server sends only values setStat accepts (stat_value_valid).
// ADD_STAT has the same layout with the change in place of the value (stat_change_valid).
// SET_STAT may carry a seventh byte of flags: bit 0, the body falls at once (health 0 only,
// Player.setHealth(0, { fall: true })); a six-byte SET_STAT has none.
inline constexpr std::uint8_t kSetStatTag = 15;
inline constexpr std::uint8_t kAddStatTag = 54;
inline constexpr float kMaxStatValue = 1000000.0f;
// Positive health at or below this is refused: the game's state setter treats it as zero.
inline constexpr float kMinPositiveHealth = 0.00001f;

struct StatRequest {
  std::uint8_t stat = 0;
  // The new value, or with `relative` the change added to the current one.
  float value = 0.0f;
  // ADD_STAT (Player.addStat): the owner's game adds `value` to what its state holds.
  bool relative = false;
  // Health 0 only: the owner's game drops the body at once instead of letting it stand first.
  bool fall = false;

  friend bool operator==(const StatRequest& a, const StatRequest& b) {
    return a.stat == b.stat && a.value == b.value && a.relative == b.relative && a.fall == b.fall;
  }
};

inline bool stat_value_valid(std::uint8_t stat, float value) {
  if (stat >= kWritableStateCount || !std::isfinite(value) || value < 0.0f ||
      value > kMaxStatValue) {
    return false;
  }
  return !(stat == 0 && value > 0.0f && value <= kMinPositiveHealth);
}

// A change addStat may send: a writable state and a finite change of at most kMaxStatValue
// either way.
inline bool stat_change_valid(std::uint8_t stat, float change) {
  return stat < kWritableStateCount && std::isfinite(change) && change >= -kMaxStatValue &&
         change <= kMaxStatValue;
}

inline constexpr std::uint8_t kStatFlagFall = 1;

inline bool stat_request_valid(const StatRequest& request) {
  if (request.fall && (request.relative || request.stat != 0 || request.value != 0.0f)) {
    return false;
  }
  if (request.relative) {
    return stat_change_valid(request.stat, request.value);
  }
  return stat_value_valid(request.stat, request.value);
}

namespace detail {

inline std::vector<std::uint8_t> encode_stat_message(std::uint8_t tag, const StatRequest& request) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &request.value, 4);
  std::vector<std::uint8_t> out = {tag,
                                   request.stat,
                                   std::uint8_t(bits),
                                   std::uint8_t(bits >> 8),
                                   std::uint8_t(bits >> 16),
                                   std::uint8_t(bits >> 24)};
  if (request.fall) {
    out.push_back(kStatFlagFall);
  }
  return out;
}

inline bool decode_stat_message(std::uint8_t tag, const std::vector<std::uint8_t>& p,
                                StatRequest& request) {
  const bool flagged = p.size() == 7 && tag == kSetStatTag;
  if ((p.size() != 6 && !flagged) || p[0] != tag || (flagged && p[6] != kStatFlagFall)) {
    return false;
  }
  const std::uint32_t bits = std::uint32_t(p[2]) | std::uint32_t(p[3]) << 8 |
                             std::uint32_t(p[4]) << 16 | std::uint32_t(p[5]) << 24;
  StatRequest decoded;
  decoded.stat = p[1];
  std::memcpy(&decoded.value, &bits, 4);
  decoded.relative = tag == kAddStatTag;
  decoded.fall = flagged;
  if (!stat_request_valid(decoded)) {
    return false;
  }
  request = decoded;
  return true;
}

}  // namespace detail

inline std::vector<std::uint8_t> encode_set_stat(const StatRequest& request) {
  if (request.relative || !stat_request_valid(request)) {
    return {};
  }
  return detail::encode_stat_message(kSetStatTag, request);
}

inline bool decode_set_stat(const std::vector<std::uint8_t>& p, StatRequest& request) {
  return detail::decode_stat_message(kSetStatTag, p, request);
}

inline std::vector<std::uint8_t> encode_add_stat(const StatRequest& request) {
  if (!request.relative || !stat_change_valid(request.stat, request.value)) {
    return {};
  }
  return detail::encode_stat_message(kAddStatTag, request);
}

inline bool decode_add_stat(const std::vector<std::uint8_t>& p, StatRequest& request) {
  return detail::decode_stat_message(kAddStatTag, p, request);
}

// The message that carries a request: SET_STAT or ADD_STAT; empty for one that cannot travel.
inline std::vector<std::uint8_t> encode_stat_request(const StatRequest& request) {
  if (request.relative) {
    return encode_add_stat(request);
  }
  return encode_set_stat(request);
}

}  // namespace kcd2s::integration
