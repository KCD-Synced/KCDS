#pragma once
#include <cstdint>
#include <vector>

#include "kcd2s/core/bits.hpp"

// A player carrying another player's knocked-out body on the shoulder (issue),
// the game's own corpse carry.
//
//   BODY_CARRY_REPORT : the carrier's client -> server, on the control channel (0): its game
//                       picked up (Started) or let go of (Ended) the body of a player.
//   BODY_CARRY        : server -> clients: a carry the server accepted (Carry: everybody but the
//                       carrier shows it, the carried player's own game too), one that ended
//                       (Drop), or an order to the carrier to put the body down (PutDown: the
//                       game's put-down; Refuse: the server did not accept the pick-up).
namespace kcd2s::integration {

inline constexpr std::uint8_t kBodyCarryReportTag = 48;
inline constexpr std::uint8_t kBodyCarryTag = 49;

struct BodyCarryReport {
  enum class Kind : std::uint8_t {
    Started = 1,
    Ended = 2,
  };
  Kind kind = Kind::Started;
  std::uint32_t carried = 0;  // the carried player's network id

  friend bool operator==(const BodyCarryReport& a, const BodyCarryReport& b) {
    return a.kind == b.kind && a.carried == b.carried;
  }
};

struct BodyCarry {
  enum class Kind : std::uint8_t {
    Carry = 1,
    Drop = 2,
    PutDown = 3,
    Refuse = 4,
  };
  Kind kind = Kind::Carry;
  std::uint32_t carrier = 0;
  std::uint32_t carried = 0;

  friend bool operator==(const BodyCarry& a, const BodyCarry& b) {
    return a.kind == b.kind && a.carrier == b.carrier && a.carried == b.carried;
  }
};

// BODY_CARRY_REPORT: [tag][u8 kind][varuint carried]; empty for an unknown kind or no player.
inline std::vector<std::uint8_t> encode_body_carry_report(const BodyCarryReport& report) {
  const auto kind = std::uint8_t(report.kind);
  if ((kind != 1 && kind != 2) || report.carried == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kBodyCarryReportTag, 8);
  w.write_bits(kind, 8);
  w.write_varuint(report.carried);
  return w.bytes();
}

inline bool decode_body_carry_report(const std::vector<std::uint8_t>& p, BodyCarryReport& report) {
  if (p.empty() || p[0] != kBodyCarryReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const auto kind = r.read_bits(8);
  const std::uint64_t carried = r.read_varuint();
  if (r.overrun() || r.bits_remaining() != 0 || (kind != 1 && kind != 2) || carried == 0 ||
      carried > 0xFFFFFFFFu) {
    return false;
  }
  report.kind = BodyCarryReport::Kind(kind);
  report.carried = std::uint32_t(carried);
  return true;
}

// BODY_CARRY: [tag][u8 kind][varuint carrier][varuint carried]; empty for an unknown kind or a
// missing player.
inline std::vector<std::uint8_t> encode_body_carry(const BodyCarry& carry) {
  const auto kind = std::uint8_t(carry.kind);
  if (kind < 1 || kind > 4 || carry.carrier == 0 || carry.carried == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kBodyCarryTag, 8);
  w.write_bits(kind, 8);
  w.write_varuint(carry.carrier);
  w.write_varuint(carry.carried);
  return w.bytes();
}

inline bool decode_body_carry(const std::vector<std::uint8_t>& p, BodyCarry& carry) {
  if (p.empty() || p[0] != kBodyCarryTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  const auto kind = r.read_bits(8);
  const std::uint64_t carrier = r.read_varuint();
  const std::uint64_t carried = r.read_varuint();
  if (r.overrun() || r.bits_remaining() != 0 || kind < 1 || kind > 4 || carrier == 0 ||
      carried == 0 || carrier > 0xFFFFFFFFu || carried > 0xFFFFFFFFu) {
    return false;
  }
  carry.kind = BodyCarry::Kind(kind);
  carry.carrier = std::uint32_t(carrier);
  carry.carried = std::uint32_t(carried);
  return true;
}

}  // namespace kcd2s::integration
