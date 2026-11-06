#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Crafting at a station, asked of and told to the server. On the control
// channel (0).
//   CRAFT_REPORT  : the player's own client -> server. `Ask`: the player's game is about to open a
//                   station (an alchemy table, a smithy) and waits for CRAFT_VERDICT; the server's
//                   resources may refuse (craftingStarting). `Started`: the minigame runs at the
//                   station. `Ended`: it is over, and why ("cancelled": the player left it).
//                   `Recipe`: the smithing recipe the player chose at the station (its id in
//                   `reason`); the server's resources may refuse it (craftingStarting), and the
//                   server then closes the station. `Prepare`: the player's game is about to cook,
//                   smoke or dry at a fire, smokehouse or rack (`reason`: "cooking", "smoking" or
//                   "drying"; `station`: where) and waits for CRAFT_VERDICT; the server's resources
//                   may refuse (playerPreparing). Its requests count from 0x40000000.
//   CRAFT_VERDICT : server -> the player's own client, whether an Ask may go on. Asks are numbered
//                   from 1: a refusal of request 0 is the server closing the station the player
//                   works at (Crafting.cancel).
// A station is named by where it stands; the server finds it in its catalog.
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kCraftReportTag = 129;
inline constexpr std::uint8_t kCraftVerdictTag = 130;
// Reports a client may send in a second, and the longest reason.
inline constexpr unsigned kMaxCraftReportsPerSecond = 8;
inline constexpr std::size_t kMaxCraftReasonBytes = 48;
inline constexpr float kCraftWorldReach = 100000.0f;

enum class CraftKind : std::uint8_t { Alchemy = 0, Smithing = 1 };

struct CraftReport {
  enum class Kind : std::uint8_t {
    Ask = 1,
    Started = 2,
    Ended = 3,
    Recipe = 4,
    Failed = 5,
    Prepare = 6
  };
  Kind kind = Kind::Ask;
  std::uint32_t request = 0;  // Ask: the answer names it
  CraftKind craft = CraftKind::Alchemy;
  std::array<float, 3> station{};
  std::string reason;  // Ended: why; Recipe: the recipe's id
  bool operator==(const CraftReport&) const = default;
};

struct CraftVerdict {
  std::uint32_t request = 0;
  bool allowed = false;
  bool operator==(const CraftVerdict&) const = default;
};

namespace crafting_detail {

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

// Lowercase letters, digits and '_', at most kMaxCraftReasonBytes.
inline bool reason_valid(const std::string& reason) {
  if (reason.size() > kMaxCraftReasonBytes) {
    return false;
  }
  for (const char c : reason) {
    const bool letter = c >= 'a' && c <= 'z';
    const bool upper = c >= 'A' && c <= 'Z';
    const bool digit = c >= '0' && c <= '9';
    if (!letter && !upper && !digit && c != '_') {
      return false;
    }
  }
  return true;
}

inline bool station_valid(const std::array<float, 3>& station) {
  for (const float value : station) {
    if (!std::isfinite(value) || std::fabs(value) > kCraftWorldReach) {
      return false;
    }
  }
  return true;
}

}  // namespace crafting_detail

// CRAFT_REPORT: [tag][u8 kind][varuint request][u8 craft][f32 x3 station][varuint n][n reason].
// Empty for a report that cannot travel.
inline std::vector<std::uint8_t> encode_craft_report(const CraftReport& report) {
  if (report.kind < CraftReport::Kind::Ask || report.kind > CraftReport::Kind::Prepare ||
      report.craft > CraftKind::Smithing || !crafting_detail::station_valid(report.station) ||
      !crafting_detail::reason_valid(report.reason)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kCraftReportTag, 8);
  w.write_bits(std::uint8_t(report.kind), 8);
  w.write_varuint(report.request);
  w.write_bits(std::uint8_t(report.craft), 8);
  for (const float value : report.station) {
    crafting_detail::write_float(w, value);
  }
  w.write_varuint(report.reason.size());
  w.write_bytes(reinterpret_cast<const std::uint8_t*>(report.reason.data()), report.reason.size());
  return w.bytes();
}

inline bool decode_craft_report(const std::vector<std::uint8_t>& p, CraftReport& report) {
  if (p.empty() || p[0] != kCraftReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  CraftReport decoded;
  const std::uint64_t kind = r.read_bits(8);
  const std::uint64_t request = r.read_varuint();
  const std::uint64_t craft = r.read_bits(8);
  for (float& value : decoded.station) {
    value = crafting_detail::read_float(r);
  }
  const std::uint64_t length = r.read_varuint();
  if (r.overrun() || kind < 1 || kind > 6 || request > 0xFFFFFFFFu || craft > 1 ||
      length > kMaxCraftReasonBytes) {
    return false;
  }
  decoded.reason.resize(length);
  for (char& c : decoded.reason) {
    c = char(r.read_bits(8));
  }
  decoded.kind = CraftReport::Kind(kind);
  decoded.request = std::uint32_t(request);
  decoded.craft = CraftKind(craft);
  if (r.overrun() || r.bits_remaining() != 0 || !crafting_detail::station_valid(decoded.station) ||
      !crafting_detail::reason_valid(decoded.reason)) {
    return false;
  }
  report = std::move(decoded);
  return true;
}

// CRAFT_VERDICT: [tag][varuint request][u8 allowed].
inline std::vector<std::uint8_t> encode_craft_verdict(const CraftVerdict& verdict) {
  core::BitWriter w;
  w.write_bits(kCraftVerdictTag, 8);
  w.write_varuint(verdict.request);
  w.write_bits(verdict.allowed ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_craft_verdict(const std::vector<std::uint8_t>& p, CraftVerdict& verdict) {
  if (p.empty() || p[0] != kCraftVerdictTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t request = r.read_varuint();
  const std::uint64_t allowed = r.read_bits(8);
  if (r.overrun() || r.bits_remaining() != 0 || request > 0xFFFFFFFFu || allowed > 1) {
    return false;
  }
  verdict.request = std::uint32_t(request);
  verdict.allowed = allowed == 1;
  return true;
}

}  // namespace kcd2s::integration
