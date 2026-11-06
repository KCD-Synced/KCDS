#pragma once
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/guid.hpp"

// An item a player carries in their arms (Player.holdItem, dropHeldItem, heldItem),
// the game's own carry (its carry action, the one its sacks use).
//
// The owner publishes what it carries in the NetworkPlayer field "carry": the item class's GUID
// (guid_text's form), "" for none. The server raises the carry events from it and every other
// client shows the carry on the player's replica.
//
//   CARRY_ORDER  : server -> the player's client, on the control channel (0): carry a new item of
//                  a class, take up a carryable stack lying in the world (its network id), or
//                  put the carried item down (with the game's put-down, or let go at once).
//   CARRY_REPORT : the player's client -> server: its carry ended with the item let go in the
//                  world, of this class, where it is now; the server lays a carryable prop there.
namespace kcd2s::integration {

inline constexpr std::uint8_t kCarryOrderTag = 32;
inline constexpr std::uint8_t kCarryReportTag = 33;

enum class CarryOrderKind : std::uint8_t {
  Carry = 1,
  PutDown = 2,
  LetGo = 3,
  CarryStack = 4,
};

struct CarryOrder {
  CarryOrderKind kind = CarryOrderKind::PutDown;
  Guid item{};              // the class to carry (Carry only)
  std::uint32_t stack = 0;  // the stack to take up (CarryStack only)

  friend bool operator==(const CarryOrder& a, const CarryOrder& b) {
    return a.kind == b.kind && (a.kind != CarryOrderKind::Carry || a.item == b.item) &&
           (a.kind != CarryOrderKind::CarryStack || a.stack == b.stack);
  }
};

// CARRY_ORDER: [tag][u8 kind][16 bytes class GUID, Carry only][varuint stack, CarryStack only].
inline std::vector<std::uint8_t> encode_carry_order(const CarryOrder& order) {
  if (order.kind != CarryOrderKind::Carry && order.kind != CarryOrderKind::PutDown &&
      order.kind != CarryOrderKind::LetGo && order.kind != CarryOrderKind::CarryStack) {
    return {};
  }
  if (order.kind == CarryOrderKind::CarryStack && order.stack == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kCarryOrderTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  if (order.kind == CarryOrderKind::Carry) {
    for (const std::uint8_t byte : order.item) {
      w.write_bits(byte, 8);
    }
  }
  if (order.kind == CarryOrderKind::CarryStack) {
    w.write_varuint(order.stack);
  }
  return w.bytes();
}

inline bool decode_carry_order(const std::vector<std::uint8_t>& p, CarryOrder& order) {
  if (p.empty() || p[0] != kCarryOrderTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  CarryOrder decoded;
  const auto kind = std::uint8_t(r.read_bits(8));
  if (kind < std::uint8_t(CarryOrderKind::Carry) ||
      kind > std::uint8_t(CarryOrderKind::CarryStack)) {
    return false;
  }
  decoded.kind = CarryOrderKind(kind);
  if (decoded.kind == CarryOrderKind::Carry) {
    for (std::uint8_t& byte : decoded.item) {
      byte = std::uint8_t(r.read_bits(8));
    }
  }
  std::uint64_t stack = 0;
  if (decoded.kind == CarryOrderKind::CarryStack) {
    stack = r.read_varuint();
  }
  if (r.overrun() || r.bits_remaining() != 0 || stack > 0xFFFFFFFFu ||
      (decoded.kind == CarryOrderKind::CarryStack && stack == 0)) {
    return false;
  }
  decoded.stack = std::uint32_t(stack);
  order = decoded;
  return true;
}

// Where the item a carry let go of lies in the world (CARRY_REPORT).
struct CarryPutDown {
  Guid item{};
  float position[3] = {};

  friend bool operator==(const CarryPutDown& a, const CarryPutDown& b) {
    return a.item == b.item && a.position[0] == b.position[0] && a.position[1] == b.position[1] &&
           a.position[2] == b.position[2];
  }
};

// CARRY_REPORT: [tag][16 bytes class GUID][3 x u32 position float bits]; empty for a position
// that is not finite.
inline std::vector<std::uint8_t> encode_carry_put_down(const CarryPutDown& put_down) {
  core::BitWriter w;
  w.write_bits(kCarryReportTag, 8);
  for (const std::uint8_t byte : put_down.item) {
    w.write_bits(byte, 8);
  }
  for (const float axis : put_down.position) {
    if (!std::isfinite(axis)) {
      return {};
    }
    std::uint32_t bits = 0;
    std::memcpy(&bits, &axis, sizeof(bits));
    w.write_bits(bits, 32);
  }
  return w.bytes();
}

inline bool decode_carry_put_down(const std::vector<std::uint8_t>& p, CarryPutDown& put_down) {
  if (p.empty() || p[0] != kCarryReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  CarryPutDown decoded;
  for (std::uint8_t& byte : decoded.item) {
    byte = std::uint8_t(r.read_bits(8));
  }
  for (float& axis : decoded.position) {
    const auto bits = std::uint32_t(r.read_bits(32));
    std::memcpy(&axis, &bits, sizeof(axis));
  }
  if (r.overrun() || r.bits_remaining() != 0) {
    return false;
  }
  for (const float axis : decoded.position) {
    if (!std::isfinite(axis)) {
      return false;
    }
  }
  put_down = decoded;
  return true;
}

// The "carry" field of a class carried, "" for none.
inline std::string carry_field(const Guid* item) {
  return item ? guid_text(*item) : std::string();
}

// The class a "carry" field names; false for "" or anything that is not a GUID.
inline bool parse_carry_field(const std::string& field, Guid& item) {
  return !field.empty() && parse_guid(field, item);
}

}  // namespace kcd2s::integration
