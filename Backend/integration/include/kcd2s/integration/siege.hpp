#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Siege engines: the server's SiegeEngine, a trebuchet or a cannon.
// The server owns the cycle, the operator, the ammunition and the outcome of every shot; the
// clients draw the engine and its stones, offer its use to a player near it and report where a
// stone came down. On the control channel (0).
//
//   SIEGE_ENGINE : server -> a ready player's client:
//                  [tag][u8 kind][varuint engine id] then by kind
//                    Spawn   : [u8 engine kind][3 floats position][varuint dimension][state]
//                    Update  : [state]
//                    Destroy : nothing
//                  state: [u8 cycle][float seconds][float speed][4 floats rotation (x, y, z, w)]
//                  [3 floats target][u8 flags: 1 aim guide, 2 wrecked, 4 usable, 8 operated,
//                  16 operated by this client's player]
//   SIEGE_SHOT   : server -> a ready player's client in the shot's dimension:
//                  [tag][varuint engine id][varuint shot][3 floats from][3 floats target]
//                  [float flight seconds][float apex metres][u8 1 when this client reports it]
//   SIEGE_USE    : the player's own client -> server: [tag][varuint engine id][u8 action]
//                  [u8 has look][3 floats look direction when it has one]
//   SIEGE_IMPACT : the reporter's client -> server: [tag][varuint engine id][varuint shot]
//                  [3 floats position]
//
// The server writes the "this client" flags per receiver, so a client needs no player id of its
// own. Client/runtime/siege/siege_engines.hpp applies them.
namespace kcd2s::integration {

// 107 to 112 are 0.2.1's siege client ops; 111 and 112 are the ladders' and stone piles'
// (ladders_stones.hpp).
inline constexpr std::uint8_t kSiegeEngineTag = 107;
inline constexpr std::uint8_t kSiegeShotTag = 108;
inline constexpr std::uint8_t kSiegeUseTag = 109;
inline constexpr std::uint8_t kSiegeImpactTag = 110;

// Orders and shots a client holds before its game takes them (further ones are dropped).
inline constexpr std::size_t kMaxSiegeOrders = 256;
// Uses and impacts the server holds per tick before the resource host takes them, and how many
// of them one client may send in a second.
inline constexpr std::size_t kMaxSiegeReports = 256;
inline constexpr unsigned kMaxSiegeReportsPerSecond = 8;

enum class SiegeOrderKind : std::uint8_t {
  Spawn = 1,
  Update = 2,
  Destroy = 3,
};

enum class SiegeEngineKind : std::uint8_t {
  Trebuchet = 0,
  Cannon = 1,
};
inline constexpr std::uint8_t kSiegeEngineKinds = 2;

enum class SiegeCycle : std::uint8_t {
  Idle = 0,
  Drawing = 1,
  Loading = 2,
  Ready = 3,
  Firing = 4,
};
inline constexpr std::uint8_t kSiegeCycles = 5;

enum class SiegeAction : std::uint8_t {
  Operate = 0,
  Leave = 1,
  Load = 2,
  Fire = 3,
};
inline constexpr std::uint8_t kSiegeActions = 4;

struct SiegeEngineState {
  SiegeCycle cycle = SiegeCycle::Idle;
  float seconds = 0.0f;  // how long the step just entered takes; 0 when none
  float speed = 1.0f;
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> target{};  // where the aim guide points; zeros for none
  bool aim_guide = false;
  bool wrecked = false;
  bool usable = false;
  bool operated = false;  // a player works it
  bool mine = false;      // and that player is this client's

  friend bool operator==(const SiegeEngineState& a, const SiegeEngineState& b) {
    return a.cycle == b.cycle && a.seconds == b.seconds && a.speed == b.speed &&
           a.rotation == b.rotation && a.target == b.target && a.aim_guide == b.aim_guide &&
           a.wrecked == b.wrecked && a.usable == b.usable && a.operated == b.operated &&
           a.mine == b.mine;
  }
};

struct SiegeEngineOrder {
  SiegeOrderKind kind = SiegeOrderKind::Spawn;
  std::uint32_t id = 0;
  SiegeEngineKind engine = SiegeEngineKind::Trebuchet;  // Spawn
  std::array<float, 3> position{};                      // Spawn
  std::uint32_t dimension = 0;                          // Spawn
  SiegeEngineState state;                               // Spawn, Update

  friend bool operator==(const SiegeEngineOrder& a, const SiegeEngineOrder& b) {
    return a.kind == b.kind && a.id == b.id && a.engine == b.engine && a.position == b.position &&
           a.dimension == b.dimension && a.state == b.state;
  }
};

struct SiegeShot {
  std::uint32_t engine = 0;
  std::uint32_t shot = 0;
  std::array<float, 3> from{};
  std::array<float, 3> target{};
  float flight = 0.0f;  // seconds in the air
  float apex = 0.0f;    // metres above `from` at the top of the arc
  bool report = false;  // this client says where it came down

  friend bool operator==(const SiegeShot& a, const SiegeShot& b) {
    return a.engine == b.engine && a.shot == b.shot && a.from == b.from && a.target == b.target &&
           a.flight == b.flight && a.apex == b.apex && a.report == b.report;
  }
};

struct SiegeUse {
  std::uint32_t engine = 0;
  SiegeAction action = SiegeAction::Operate;
  bool has_look = false;
  std::array<float, 3> look{};  // where the player looks (a direction)

  friend bool operator==(const SiegeUse& a, const SiegeUse& b) {
    return a.engine == b.engine && a.action == b.action && a.has_look == b.has_look &&
           a.look == b.look;
  }
};

struct SiegeImpact {
  std::uint32_t engine = 0;
  std::uint32_t shot = 0;
  std::array<float, 3> position{};

  friend bool operator==(const SiegeImpact& a, const SiegeImpact& b) {
    return a.engine == b.engine && a.shot == b.shot && a.position == b.position;
  }
};

namespace siege_detail {

inline constexpr std::uint8_t kAimGuide = 1;
inline constexpr std::uint8_t kWrecked = 2;
inline constexpr std::uint8_t kUsable = 4;
inline constexpr std::uint8_t kOperated = 8;
inline constexpr std::uint8_t kMine = 16;
inline constexpr std::uint8_t kFlags = 31;
// Seconds, speeds and arcs a message may carry.
inline constexpr float kMaxSeconds = 3600.0f;
inline constexpr float kMaxSpeed = 100.0f;
inline constexpr float kMaxApex = 10000.0f;

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

inline bool in_range(float value, float max) {
  return std::isfinite(value) && value >= 0.0f && value <= max;
}

inline bool valid_state(const SiegeEngineState& s) {
  return std::uint8_t(s.cycle) < kSiegeCycles && in_range(s.seconds, kMaxSeconds) &&
         in_range(s.speed, kMaxSpeed) && finite(s.rotation) && finite(s.target) &&
         (!s.mine || s.operated);
}

inline void write_state(core::BitWriter& w, const SiegeEngineState& s) {
  w.write_bits(std::uint8_t(s.cycle), 8);
  write_float(w, s.seconds);
  write_float(w, s.speed);
  write_floats(w, s.rotation);
  write_floats(w, s.target);
  std::uint8_t flags = 0;
  if (s.aim_guide) {
    flags |= kAimGuide;
  }
  if (s.wrecked) {
    flags |= kWrecked;
  }
  if (s.usable) {
    flags |= kUsable;
  }
  if (s.operated) {
    flags |= kOperated;
  }
  if (s.mine) {
    flags |= kMine;
  }
  w.write_bits(flags, 8);
}

inline bool read_state(core::BitReader& r, SiegeEngineState& s) {
  s.cycle = SiegeCycle(r.read_bits(8));
  s.seconds = read_float(r);
  s.speed = read_float(r);
  read_floats(r, s.rotation);
  read_floats(r, s.target);
  const std::uint8_t flags = std::uint8_t(r.read_bits(8));
  if (r.overrun() || (flags & ~kFlags) != 0) {
    return false;
  }
  s.aim_guide = (flags & kAimGuide) != 0;
  s.wrecked = (flags & kWrecked) != 0;
  s.usable = (flags & kUsable) != 0;
  s.operated = (flags & kOperated) != 0;
  s.mine = (flags & kMine) != 0;
  return valid_state(s);
}

// An entity or shot id: 1 .. 2^32 - 1.
inline bool read_id(core::BitReader& r, std::uint32_t& id) {
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value == 0 || value > 0xffffffffull) {
    return false;
  }
  id = std::uint32_t(value);
  return true;
}

// Nothing but the last byte's padding may follow.
inline bool at_end(const core::BitReader& r) {
  return !r.overrun() && r.bits_remaining() < 8;
}

}  // namespace siege_detail

// Empty when the order cannot travel.
inline std::vector<std::uint8_t> encode_siege_engine(const SiegeEngineOrder& order) {
  using namespace siege_detail;
  if (order.id == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kSiegeEngineTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.id);
  switch (order.kind) {
    case SiegeOrderKind::Spawn:
      if (std::uint8_t(order.engine) >= kSiegeEngineKinds || !finite(order.position) ||
          !valid_state(order.state)) {
        return {};
      }
      w.write_bits(std::uint8_t(order.engine), 8);
      write_floats(w, order.position);
      w.write_varuint(order.dimension);
      write_state(w, order.state);
      break;
    case SiegeOrderKind::Update:
      if (!valid_state(order.state)) {
        return {};
      }
      write_state(w, order.state);
      break;
    case SiegeOrderKind::Destroy:
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_siege_engine(const std::vector<std::uint8_t>& p, SiegeEngineOrder& order) {
  using namespace siege_detail;
  if (p.size() < 3 || p[0] != kSiegeEngineTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  SiegeEngineOrder decoded;
  decoded.kind = SiegeOrderKind(r.read_bits(8));
  if (!read_id(r, decoded.id)) {
    return false;
  }
  switch (decoded.kind) {
    case SiegeOrderKind::Spawn: {
      decoded.engine = SiegeEngineKind(r.read_bits(8));
      read_floats(r, decoded.position);
      const std::uint64_t world = r.read_varuint();
      if (r.overrun() || std::uint8_t(decoded.engine) >= kSiegeEngineKinds ||
          !finite(decoded.position) || world > 0xffffffffull) {
        return false;
      }
      decoded.dimension = std::uint32_t(world);
      if (!read_state(r, decoded.state)) {
        return false;
      }
      break;
    }
    case SiegeOrderKind::Update:
      if (!read_state(r, decoded.state)) {
        return false;
      }
      break;
    case SiegeOrderKind::Destroy:
      break;
    default:
      return false;
  }
  if (!at_end(r)) {
    return false;
  }
  order = decoded;
  return true;
}

inline std::vector<std::uint8_t> encode_siege_shot(const SiegeShot& shot) {
  using namespace siege_detail;
  if (shot.engine == 0 || shot.shot == 0 || !finite(shot.from) || !finite(shot.target) ||
      !in_range(shot.flight, kMaxSeconds) || !in_range(shot.apex, kMaxApex)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kSiegeShotTag, 8);
  w.write_varuint(shot.engine);
  w.write_varuint(shot.shot);
  write_floats(w, shot.from);
  write_floats(w, shot.target);
  write_float(w, shot.flight);
  write_float(w, shot.apex);
  w.write_bits(shot.report ? 1 : 0, 8);
  return w.bytes();
}

inline bool decode_siege_shot(const std::vector<std::uint8_t>& p, SiegeShot& shot) {
  using namespace siege_detail;
  if (p.size() < 3 || p[0] != kSiegeShotTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  SiegeShot decoded;
  if (!read_id(r, decoded.engine) || !read_id(r, decoded.shot)) {
    return false;
  }
  read_floats(r, decoded.from);
  read_floats(r, decoded.target);
  decoded.flight = read_float(r);
  decoded.apex = read_float(r);
  const std::uint64_t report = r.read_bits(8);
  if (!at_end(r) || report > 1 || !finite(decoded.from) || !finite(decoded.target) ||
      !in_range(decoded.flight, kMaxSeconds) || !in_range(decoded.apex, kMaxApex)) {
    return false;
  }
  decoded.report = report == 1;
  shot = decoded;
  return true;
}

inline std::vector<std::uint8_t> encode_siege_use(const SiegeUse& use) {
  using namespace siege_detail;
  if (use.engine == 0 || std::uint8_t(use.action) >= kSiegeActions ||
      (use.has_look && !finite(use.look))) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kSiegeUseTag, 8);
  w.write_varuint(use.engine);
  w.write_bits(std::uint8_t(use.action), 8);
  w.write_bits(use.has_look ? 1 : 0, 8);
  if (use.has_look) {
    write_floats(w, use.look);
  }
  return w.bytes();
}

inline bool decode_siege_use(const std::vector<std::uint8_t>& p, SiegeUse& use) {
  using namespace siege_detail;
  if (p.size() < 3 || p[0] != kSiegeUseTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  SiegeUse decoded;
  if (!read_id(r, decoded.engine)) {
    return false;
  }
  decoded.action = SiegeAction(r.read_bits(8));
  const std::uint64_t has_look = r.read_bits(8);
  if (r.overrun() || std::uint8_t(decoded.action) >= kSiegeActions || has_look > 1) {
    return false;
  }
  decoded.has_look = has_look == 1;
  if (decoded.has_look) {
    read_floats(r, decoded.look);
    if (!finite(decoded.look)) {
      return false;
    }
  }
  if (!at_end(r)) {
    return false;
  }
  use = decoded;
  return true;
}

inline std::vector<std::uint8_t> encode_siege_impact(const SiegeImpact& impact) {
  using namespace siege_detail;
  if (impact.engine == 0 || impact.shot == 0 || !finite(impact.position)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kSiegeImpactTag, 8);
  w.write_varuint(impact.engine);
  w.write_varuint(impact.shot);
  write_floats(w, impact.position);
  return w.bytes();
}

inline bool decode_siege_impact(const std::vector<std::uint8_t>& p, SiegeImpact& impact) {
  using namespace siege_detail;
  if (p.size() < 3 || p[0] != kSiegeImpactTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  SiegeImpact decoded;
  if (!read_id(r, decoded.engine) || !read_id(r, decoded.shot)) {
    return false;
  }
  read_floats(r, decoded.position);
  if (!at_end(r) || !finite(decoded.position)) {
    return false;
  }
  impact = decoded;
  return true;
}

}  // namespace kcd2s::integration
