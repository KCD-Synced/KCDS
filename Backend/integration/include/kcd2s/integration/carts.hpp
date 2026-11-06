#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Carts and wagons: the server holds them and who sits where, every
// client in their dimension shows them, and the driver's client runs the cart.
//
//   CART        : server -> a ready player's client, on the control channel (0):
//                 [tag][u8 kind][varuint cart id] then by kind
//                   Spawn    : [text blueprint][3 floats position][4 floats rotation]
//                              [varuint dimension][float max speed]
//                   Destroy  : nothing
//                   Seat     : [varuint player][u8 seat]   the player climbs into the seat
//                   Settled  : [varuint player][u8 seat]   their climb on ended: held on the seat
//                   ClimbOn  : [varuint player][u8 seat]   their climb's clip starts
//                   ScootOff : [varuint player][u8 seat]   they move along the bench to where their
//                                                          climb down starts
//                   ClimbOff : [varuint player][u8 seat]   their climb down starts: not held
//                   ReinsTaken : [varuint player][u8 seat]  the climbing driver's hands take the
//                   reins ReinsGone  : [varuint player][u8 seat]  the climbing driver's hands let
//                   them go Leave    : [varuint player][u8 seat]   the player is down Deny     :
//                   [varuint player][u8 seat]   the player's own enter was refused Teleport : [3
//                   floats position][4 floats rotation] Speed    : [float max speed] Pose     : [3
//                   floats position][4 floats rotation][u8 pace][varuint driver]
//                              the driver's report, for every client but the driver's
//                   Load     : [text load]   what the cart carries (its name in the client's
//                              catalog of the game's loaded carts; empty for nothing)
//   CART_REPORT : a ready client -> server, on the control channel (0):
//                 [tag][u8 kind][varuint cart id] then by kind
//                   Enter : [u8 seat]
//                   Leave : nothing
//                   Drive : [3 floats position][4 floats rotation][u8 pace]
//                   AtStart : nothing   the player has walked to where their climb starts
//
// Client/runtime/carts/placed_carts.hpp applies CART and makes the reports.
namespace kcd2s::integration {

// 69 and 70 are the world time and weather, 71 to 75 the doors, 76 and 77 the props
// and effects.
inline constexpr std::uint8_t kCartTag = 78;
inline constexpr std::uint8_t kCartReportTag = 79;

inline constexpr std::size_t kMaxCartBlueprintBytes = 64;
inline constexpr float kMaxCartSpeed = 15.0f;
// Orders a client holds before its game takes them (further ones are dropped).
inline constexpr std::size_t kMaxCartOrders = 512;

enum class CartKind : std::uint8_t {
  Spawn = 1,
  Destroy = 2,
  Seat = 3,
  Leave = 4,
  Deny = 5,
  Teleport = 6,
  Speed = 7,
  Pose = 8,
  Load = 9,
  Settled = 10,
  ClimbOff = 11,
  ReinsTaken = 12,
  ReinsGone = 13,
  ClimbOn = 14,
  ScootOff = 15,
};

enum class CartReportKind : std::uint8_t {
  Enter = 1,
  Leave = 2,
  Drive = 3,
  AtStart = 4,
};

// The seats of the server's catalog (carts/carts.cjs), in this order.
enum class CartSeat : std::uint8_t {
  Driver = 0,
  Bench = 1,
  RightBack = 2,
  LeftBack = 3,
  RightFront = 4,
  LeftFront = 5,
  RightMiddle = 6,
  LeftMiddle = 7,
};

enum class CartPace : std::uint8_t {
  Stand = 0,
  Walk = 1,
  Trot = 2,
  Reverse = 3,
};

inline constexpr std::array<const char*, 8> kCartSeatNames{"driver",      "bench",      "rightBack",
                                                           "leftBack",    "rightFront", "leftFront",
                                                           "rightMiddle", "leftMiddle"};
inline constexpr std::array<const char*, 4> kCartPaceNames{"stand", "walk", "trot", "reverse"};

inline std::optional<CartSeat> cart_seat_of(const std::string& name) {
  for (std::size_t i = 0; i < kCartSeatNames.size(); ++i) {
    if (name == kCartSeatNames[i]) {
      return CartSeat(i);
    }
  }
  return std::nullopt;
}

inline const char* cart_seat_name(CartSeat seat) {
  const auto index = std::size_t(seat);
  return index < kCartSeatNames.size() ? kCartSeatNames[index] : "";
}

inline std::optional<CartPace> cart_pace_of(const std::string& name) {
  for (std::size_t i = 0; i < kCartPaceNames.size(); ++i) {
    if (name == kCartPaceNames[i]) {
      return CartPace(i);
    }
  }
  return std::nullopt;
}

inline const char* cart_pace_name(CartPace pace) {
  const auto index = std::size_t(pace);
  return index < kCartPaceNames.size() ? kCartPaceNames[index] : "";
}

struct CartOrder {
  CartKind kind = CartKind::Spawn;
  std::uint32_t id = 0;
  std::string blueprint;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  std::uint32_t dimension = 0;
  float max_speed = 0.0f;
  std::uint32_t player = 0;  // Seat, Leave, Deny: the rider; Pose: the driver
  CartSeat seat = CartSeat::Driver;
  CartPace pace = CartPace::Stand;

  friend bool operator==(const CartOrder& a, const CartOrder& b) {
    return a.kind == b.kind && a.id == b.id && a.blueprint == b.blueprint &&
           a.position == b.position && a.rotation == b.rotation && a.dimension == b.dimension &&
           a.max_speed == b.max_speed && a.player == b.player && a.seat == b.seat &&
           a.pace == b.pace;
  }
};

struct CartReport {
  CartReportKind kind = CartReportKind::Drive;
  std::uint32_t cart = 0;
  CartSeat seat = CartSeat::Driver;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  CartPace pace = CartPace::Stand;

  friend bool operator==(const CartReport& a, const CartReport& b) {
    return a.kind == b.kind && a.cart == b.cart && a.seat == b.seat && a.position == b.position &&
           a.rotation == b.rotation && a.pace == b.pace;
  }
};

namespace cart_detail {

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

template <std::size_t N>
void write_floats(core::BitWriter& w, const std::array<float, N>& values) {
  for (const float value : values) {
    write_float(w, value);
  }
}

template <std::size_t N>
void read_floats(core::BitReader& r, std::array<float, N>& values) {
  for (float& value : values) {
    value = read_float(r);
  }
}

template <std::size_t N>
bool finite(const std::array<float, N>& values) {
  for (const float value : values) {
    if (!std::isfinite(value)) {
      return false;
    }
  }
  return true;
}

inline bool valid_speed(float speed) {
  return std::isfinite(speed) && speed >= 0.0f && speed <= kMaxCartSpeed;
}

inline bool valid_seat(CartSeat seat) {
  return std::size_t(seat) < kCartSeatNames.size();
}

inline bool valid_pace(CartPace pace) {
  return std::size_t(pace) < kCartPaceNames.size();
}

// Printable ASCII only.
inline bool printable(const std::string& text) {
  if (text.empty() || text.size() > kMaxCartBlueprintBytes) {
    return false;
  }
  for (const char c : text) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

// A load's name: printable ASCII, or empty for none.
inline bool valid_load(const std::string& text) {
  return text.empty() || printable(text);
}

inline bool read_id(core::BitReader& r, std::uint32_t& id) {
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value == 0 || value > 0xffffffffull) {
    return false;
  }
  id = std::uint32_t(value);
  return true;
}

}  // namespace cart_detail

inline std::vector<std::uint8_t> encode_cart(const CartOrder& order) {
  using namespace cart_detail;
  if (order.id == 0 || !finite(order.position) || !finite(order.rotation)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kCartTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.id);
  switch (order.kind) {
    case CartKind::Spawn:
      if (!printable(order.blueprint) || !valid_speed(order.max_speed)) {
        return {};
      }
      w.write_varuint(order.blueprint.size());
      for (const char c : order.blueprint) {
        w.write_bits(std::uint8_t(c), 8);
      }
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      w.write_varuint(order.dimension);
      write_float(w, order.max_speed);
      break;
    case CartKind::Destroy:
      break;
    case CartKind::Seat:
    case CartKind::Settled:
    case CartKind::ClimbOff:
    case CartKind::ReinsTaken:
    case CartKind::ReinsGone:
    case CartKind::ClimbOn:
    case CartKind::ScootOff:
    case CartKind::Leave:
    case CartKind::Deny:
      if (order.player == 0 || !valid_seat(order.seat)) {
        return {};
      }
      w.write_varuint(order.player);
      w.write_bits(std::uint8_t(order.seat), 8);
      break;
    case CartKind::Teleport:
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      break;
    case CartKind::Speed:
      if (!valid_speed(order.max_speed)) {
        return {};
      }
      write_float(w, order.max_speed);
      break;
    case CartKind::Pose:
      if (order.player == 0 || !valid_pace(order.pace)) {
        return {};
      }
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      w.write_bits(std::uint8_t(order.pace), 8);
      w.write_varuint(order.player);
      break;
    case CartKind::Load:
      if (!valid_load(order.blueprint)) {
        return {};
      }
      w.write_varuint(order.blueprint.size());
      for (const char c : order.blueprint) {
        w.write_bits(std::uint8_t(c), 8);
      }
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_cart(const std::vector<std::uint8_t>& p, CartOrder& order) {
  using namespace cart_detail;
  if (p.size() < 3 || p[0] != kCartTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  CartOrder decoded;
  decoded.kind = CartKind(r.read_bits(8));
  if (!read_id(r, decoded.id)) {
    return false;
  }
  bool ok = true;
  switch (decoded.kind) {
    case CartKind::Spawn: {
      const std::uint64_t size = r.read_varuint();
      if (r.overrun() || size > kMaxCartBlueprintBytes) {
        return false;
      }
      for (std::uint64_t i = 0; i < size; ++i) {
        decoded.blueprint.push_back(char(r.read_bits(8)));
      }
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      const std::uint64_t world = r.read_varuint();
      ok = world <= 0xffffffffull;
      decoded.dimension = std::uint32_t(world);
      decoded.max_speed = read_float(r);
      ok = ok && printable(decoded.blueprint) && valid_speed(decoded.max_speed);
      break;
    }
    case CartKind::Destroy:
      break;
    case CartKind::Seat:
    case CartKind::Settled:
    case CartKind::ClimbOff:
    case CartKind::ReinsTaken:
    case CartKind::ReinsGone:
    case CartKind::ClimbOn:
    case CartKind::ScootOff:
    case CartKind::Leave:
    case CartKind::Deny:
      ok = read_id(r, decoded.player);
      decoded.seat = CartSeat(r.read_bits(8));
      ok = ok && valid_seat(decoded.seat);
      break;
    case CartKind::Teleport:
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      break;
    case CartKind::Speed:
      decoded.max_speed = read_float(r);
      ok = valid_speed(decoded.max_speed);
      break;
    case CartKind::Pose:
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      decoded.pace = CartPace(r.read_bits(8));
      ok = valid_pace(decoded.pace) && read_id(r, decoded.player);
      break;
    case CartKind::Load: {
      const std::uint64_t size = r.read_varuint();
      if (r.overrun() || size > kMaxCartBlueprintBytes) {
        return false;
      }
      for (std::uint64_t i = 0; i < size; ++i) {
        decoded.blueprint.push_back(char(r.read_bits(8)));
      }
      ok = valid_load(decoded.blueprint);
      break;
    }
    default:
      return false;
  }
  if (!ok || r.overrun() || !finite(decoded.position) || !finite(decoded.rotation)) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

inline std::vector<std::uint8_t> encode_cart_report(const CartReport& report) {
  using namespace cart_detail;
  if (report.cart == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kCartReportTag, 8);
  w.write_bits(std::uint8_t(report.kind), 8);
  w.write_varuint(report.cart);
  switch (report.kind) {
    case CartReportKind::Enter:
      if (!valid_seat(report.seat)) {
        return {};
      }
      w.write_bits(std::uint8_t(report.seat), 8);
      break;
    case CartReportKind::Leave:
    case CartReportKind::AtStart:
      break;
    case CartReportKind::Drive:
      if (!finite(report.position) || !finite(report.rotation) || !valid_pace(report.pace)) {
        return {};
      }
      write_floats(w, report.position);
      write_floats(w, report.rotation);
      w.write_bits(std::uint8_t(report.pace), 8);
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_cart_report(const std::vector<std::uint8_t>& p, CartReport& report) {
  using namespace cart_detail;
  if (p.size() < 3 || p[0] != kCartReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  CartReport decoded;
  decoded.kind = CartReportKind(r.read_bits(8));
  if (!read_id(r, decoded.cart)) {
    return false;
  }
  bool ok = true;
  switch (decoded.kind) {
    case CartReportKind::Enter:
      decoded.seat = CartSeat(r.read_bits(8));
      ok = valid_seat(decoded.seat);
      break;
    case CartReportKind::Leave:
    case CartReportKind::AtStart:
      break;
    case CartReportKind::Drive:
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      decoded.pace = CartPace(r.read_bits(8));
      ok = valid_pace(decoded.pace);
      break;
    default:
      return false;
  }
  if (!ok || r.overrun() || !finite(decoded.position) || !finite(decoded.rotation)) {
    return false;
  }
  report = decoded;
  return true;
}

}  // namespace kcd2s::integration
