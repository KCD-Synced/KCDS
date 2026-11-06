#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Herb gathering: a player's pick asked of the server, and the plants picked in a
// dimension on every player's game. On the control channel (0).
//   GATHER_REPORT  : the player's own client -> server. The player's game picked plants and is
//                    about to give the herb: its item class, how many (as the game computes it),
//                    the plants' kind (the game's pickable area id), where they grow, the radius
//                    the pick took same-kind plants in and how many it took. The game gives
//                    nothing; the server answers with GATHER_VERDICT and gives the herb itself.
//   GATHER_VERDICT : server -> the player's own client: whether the pick stands. A refused pick's
//                    plants come back in that game.
//   GATHER_PLANTS  : server -> a client: plants picked in the player's dimension (each by an
//                    id, its kind, where and the radius it took plants in) and picks that grew
//                    back (by id). `full` replaces every pick the client was told before (a world
//                    entered or joined).
//
// Messages are byte-packed with varints (kcd2s/core/bits.hpp).

namespace kcd2s::integration {

inline constexpr std::uint8_t kGatherReportTag = 135;
inline constexpr std::uint8_t kGatherVerdictTag = 136;
inline constexpr std::uint8_t kGatherPlantsTag = 137;
// Reports a client may send in a second, the most units one pick gives, the widest pick and the
// most picks and regrowths in one GATHER_PLANTS.
inline constexpr unsigned kMaxGatherReportsPerSecond = 4;
inline constexpr std::uint32_t kMaxGatherAmount = 100;
inline constexpr std::uint32_t kMaxGatherPlants = 255;
inline constexpr float kMaxGatherRadius = 20.0f;
// A pick the server sends may cover the whole map: a kind switched off (Gathering.setNaturalSpawns)
// is one pick of it around the origin.
inline constexpr float kMaxGatherPickRadius = 100000.0f;
inline constexpr std::size_t kMaxGatherEntries = 512;
inline constexpr float kGatherWorldReach = 100000.0f;

struct GatherReport {
  std::uint32_t request = 0;
  std::array<std::uint8_t, 16> item{};  // the herb's item class GUID, in the game's byte order
  std::uint32_t amount = 0;
  std::uint8_t kind = 0;
  std::array<float, 3> position{};
  float radius = 0.0f;
  std::uint32_t plants = 0;
  bool operator==(const GatherReport&) const = default;
};

struct GatherVerdict {
  std::uint32_t request = 0;
  bool granted = false;
  bool operator==(const GatherVerdict&) const = default;
};

struct GatherPick {
  std::uint32_t id = 0;
  std::uint8_t kind = 0;
  std::array<float, 3> position{};
  float radius = 0.0f;
  bool operator==(const GatherPick&) const = default;
};

struct GatherPlants {
  bool full = false;
  std::vector<GatherPick> picked;
  std::vector<std::uint32_t> grown;  // ids of picks that grew back
  bool operator==(const GatherPlants&) const = default;
};

namespace gathering_detail {

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

inline bool position_valid(const std::array<float, 3>& position) {
  for (const float value : position) {
    if (!std::isfinite(value) || std::fabs(value) > kGatherWorldReach) {
      return false;
    }
  }
  return true;
}

inline bool radius_valid(float radius) {
  return std::isfinite(radius) && radius >= 0.0f && radius <= kMaxGatherRadius;
}

inline bool report_valid(const GatherReport& report) {
  return report.amount >= 1 && report.amount <= kMaxGatherAmount && report.plants >= 1 &&
         report.plants <= kMaxGatherPlants && position_valid(report.position) &&
         radius_valid(report.radius);
}

inline bool pick_valid(const GatherPick& pick) {
  return position_valid(pick.position) && std::isfinite(pick.radius) && pick.radius >= 0.0f &&
         pick.radius <= kMaxGatherPickRadius;
}

}  // namespace gathering_detail

// GATHER_REPORT: [tag][varuint request][16 item][varuint amount][u8 kind][f32 x3 position]
// [f32 radius][varuint plants]. Empty for a report that cannot travel.
inline std::vector<std::uint8_t> encode_gather_report(const GatherReport& report) {
  if (!gathering_detail::report_valid(report)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kGatherReportTag, 8);
  w.write_varuint(report.request);
  w.write_bytes(report.item.data(), report.item.size());
  w.write_varuint(report.amount);
  w.write_bits(report.kind, 8);
  for (const float value : report.position) {
    gathering_detail::write_float(w, value);
  }
  gathering_detail::write_float(w, report.radius);
  w.write_varuint(report.plants);
  return w.bytes();
}

inline bool decode_gather_report(const std::vector<std::uint8_t>& p, GatherReport& report) {
  if (p.empty() || p[0] != kGatherReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  GatherReport decoded;
  const std::uint64_t request = r.read_varuint();
  for (std::uint8_t& byte : decoded.item) {
    byte = std::uint8_t(r.read_bits(8));
  }
  const std::uint64_t amount = r.read_varuint();
  decoded.kind = std::uint8_t(r.read_bits(8));
  for (float& value : decoded.position) {
    value = gathering_detail::read_float(r);
  }
  decoded.radius = gathering_detail::read_float(r);
  const std::uint64_t plants = r.read_varuint();
  if (r.overrun() || r.bits_remaining() != 0 || request > 0xFFFFFFFFu ||
      amount > kMaxGatherAmount || plants > kMaxGatherPlants) {
    return false;
  }
  decoded.request = std::uint32_t(request);
  decoded.amount = std::uint32_t(amount);
  decoded.plants = std::uint32_t(plants);
  if (!gathering_detail::report_valid(decoded)) {
    return false;
  }
  report = decoded;
  return true;
}

// GATHER_VERDICT: [tag][varuint request][u8 granted].
inline std::vector<std::uint8_t> encode_gather_verdict(const GatherVerdict& verdict) {
  core::BitWriter w;
  w.write_bits(kGatherVerdictTag, 8);
  w.write_varuint(verdict.request);
  w.write_bits(verdict.granted ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_gather_verdict(const std::vector<std::uint8_t>& p, GatherVerdict& verdict) {
  if (p.empty() || p[0] != kGatherVerdictTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const std::uint64_t request = r.read_varuint();
  const std::uint64_t granted = r.read_bits(8);
  if (r.overrun() || r.bits_remaining() != 0 || request > 0xFFFFFFFFu || granted > 1) {
    return false;
  }
  verdict.request = std::uint32_t(request);
  verdict.granted = granted == 1;
  return true;
}

// GATHER_PLANTS: [tag][u8 full][varuint n][n x (varuint id, u8 kind, f32 x3, f32 radius)]
// [varuint m][m x varuint id]. Empty for plants that cannot travel.
inline std::vector<std::uint8_t> encode_gather_plants(const GatherPlants& plants) {
  if (plants.picked.size() > kMaxGatherEntries || plants.grown.size() > kMaxGatherEntries) {
    return {};
  }
  for (const GatherPick& pick : plants.picked) {
    if (!gathering_detail::pick_valid(pick)) {
      return {};
    }
  }
  core::BitWriter w;
  w.write_bits(kGatherPlantsTag, 8);
  w.write_bits(plants.full ? 1 : 0, 8);
  w.write_varuint(plants.picked.size());
  for (const GatherPick& pick : plants.picked) {
    w.write_varuint(pick.id);
    w.write_bits(pick.kind, 8);
    for (const float value : pick.position) {
      gathering_detail::write_float(w, value);
    }
    gathering_detail::write_float(w, pick.radius);
  }
  w.write_varuint(plants.grown.size());
  for (const std::uint32_t id : plants.grown) {
    w.write_varuint(id);
  }
  return w.bytes();
}

inline bool decode_gather_plants(const std::vector<std::uint8_t>& p, GatherPlants& plants) {
  if (p.empty() || p[0] != kGatherPlantsTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  GatherPlants decoded;
  const std::uint64_t full = r.read_bits(8);
  const std::uint64_t picked = r.read_varuint();
  if (r.overrun() || full > 1 || picked > kMaxGatherEntries) {
    return false;
  }
  decoded.full = full == 1;
  decoded.picked.resize(picked);
  for (GatherPick& pick : decoded.picked) {
    const std::uint64_t id = r.read_varuint();
    pick.kind = std::uint8_t(r.read_bits(8));
    for (float& value : pick.position) {
      value = gathering_detail::read_float(r);
    }
    pick.radius = gathering_detail::read_float(r);
    if (r.overrun() || id > 0xFFFFFFFFu || !gathering_detail::pick_valid(pick)) {
      return false;
    }
    pick.id = std::uint32_t(id);
  }
  const std::uint64_t grown = r.read_varuint();
  if (r.overrun() || grown > kMaxGatherEntries) {
    return false;
  }
  decoded.grown.resize(grown);
  for (std::uint32_t& id : decoded.grown) {
    const std::uint64_t value = r.read_varuint();
    if (r.overrun() || value > 0xFFFFFFFFu) {
      return false;
    }
    id = std::uint32_t(value);
  }
  if (r.bits_remaining() != 0) {
    return false;
  }
  plants = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
