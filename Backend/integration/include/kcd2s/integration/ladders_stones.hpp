#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/siege.hpp"

// Siege ladders and stone piles: the server's SiegeLadder and
// StonePile. The server owns a ladder's pose and a pile's stones; the clients show them, offer the
// push, the raise and the throw to their player and report what that player did. On the control
// channel (0).
//
//   SIEGE_LADDER : [tag][u8 kind] then by kind
//     server -> a ready player's client
//       Spawn   : [varuint ladder id][u8 ladder kind][float height][float lean degrees]
//                 [3 floats position][4 floats rotation (x, y, z, w)][varuint dimension]
//                 [u8 pose][u8 flags]
//       Pose    : [varuint ladder id][u8 pose][u8 flags]
//       Destroy : [varuint ladder id]
//       Refused : nothing (the player's push or raise was not allowed)
//     the player's own client -> server
//       Use     : [varuint ladder id][u8 action]
//       Settled : [varuint ladder id][u8 pose: down after a push, standing after a raise]
//     flags: 1 usable, 2 the step's actor is this client's player
//   STONE_PILE   : [tag][u8 kind] then by kind
//     server -> a ready player's client
//       Spawn   : [varuint pile id][3 floats position][4 floats rotation][varuint dimension]
//                 [varuint stones + 1 (0: never runs out)][u8 flags: 1 usable]
//       Update  : [varuint pile id][varuint stones + 1][u8 flags]
//       Destroy : [varuint pile id]
//     the thrower's client -> server
//       Take    : [varuint pile id]
//       Impact  : [varuint pile id][3 floats position]
//
// The server writes per receiver whether the step's actor is that client's player, so a client
// needs no player id of its own. Client/runtime/siege/siege_ladders.hpp and stone_piles.hpp apply
// them.
namespace kcd2s::integration {

// 107 to 110 are the siege engines' (siege.hpp).
inline constexpr std::uint8_t kSiegeLadderTag = 111;
inline constexpr std::uint8_t kStonePileTag = 112;

enum class LadderMessageKind : std::uint8_t {
  Spawn = 1,
  Pose = 2,
  Destroy = 3,
  Refused = 4,
  Use = 5,
  Settled = 6,
};

// siege/ladders.cjs KINDS, in this order.
enum class LadderKind : std::uint8_t {
  Siege = 0,
  Tall = 1,
  Short = 2,
  Low = 3,
};
inline constexpr std::uint8_t kLadderKinds = 4;

enum class LadderPose : std::uint8_t {
  Standing = 0,
  Pushed = 1,
  Down = 2,
  Raising = 3,
  Broken = 4,
};
inline constexpr std::uint8_t kLadderPoses = 5;

enum class LadderAction : std::uint8_t {
  Push = 0,
  Raise = 1,
};
inline constexpr std::uint8_t kLadderActions = 2;

enum class StonePileMessageKind : std::uint8_t {
  Spawn = 1,
  Update = 2,
  Destroy = 3,
  Take = 4,
  Impact = 5,
};

// A ladder may be at most this tall (metres) and lean at most this far (degrees).
inline constexpr float kMaxLadderHeight = 100.0f;
inline constexpr float kMaxLadderLean = 90.0f;
// A pile holds at most this many stones; -1 never runs out.
inline constexpr std::int64_t kMaxPileStones = 0x7fffffff;

// What the server tells a client of a ladder: Spawn, Pose, Destroy or Refused.
struct SiegeLadderOrder {
  LadderMessageKind kind = LadderMessageKind::Spawn;
  std::uint32_t id = 0;                                   // not for Refused
  LadderKind ladder = LadderKind::Siege;                  // Spawn
  float height = 0.0f;                                    // Spawn
  float lean = 0.0f;                                      // Spawn
  std::array<float, 3> position{};                        // Spawn
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};  // Spawn
  std::uint32_t dimension = 0;                            // Spawn
  LadderPose pose = LadderPose::Standing;                 // Spawn, Pose
  bool usable = false;                                    // Spawn, Pose
  bool mine = false;  // Spawn, Pose: the step's actor is this client's player

  friend bool operator==(const SiegeLadderOrder& a, const SiegeLadderOrder& b) {
    return a.kind == b.kind && a.id == b.id && a.ladder == b.ladder && a.height == b.height &&
           a.lean == b.lean && a.position == b.position && a.rotation == b.rotation &&
           a.dimension == b.dimension && a.pose == b.pose && a.usable == b.usable &&
           a.mine == b.mine;
  }
};

// What a client tells the server of a ladder: Use or Settled.
struct SiegeLadderReport {
  LadderMessageKind kind = LadderMessageKind::Use;
  std::uint32_t ladder = 0;
  LadderAction action = LadderAction::Push;  // Use
  LadderPose pose = LadderPose::Down;        // Settled: Down or Standing

  friend bool operator==(const SiegeLadderReport& a, const SiegeLadderReport& b) {
    return a.kind == b.kind && a.ladder == b.ladder && a.action == b.action && a.pose == b.pose;
  }
};

// What the server tells a client of a pile: Spawn, Update or Destroy.
struct StonePileOrder {
  StonePileMessageKind kind = StonePileMessageKind::Spawn;
  std::uint32_t id = 0;
  std::array<float, 3> position{};                        // Spawn
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};  // Spawn
  std::uint32_t dimension = 0;                            // Spawn
  std::int64_t stones = -1;  // Spawn, Update: stones left, -1 never runs out
  bool usable = false;       // Spawn, Update

  friend bool operator==(const StonePileOrder& a, const StonePileOrder& b) {
    return a.kind == b.kind && a.id == b.id && a.position == b.position &&
           a.rotation == b.rotation && a.dimension == b.dimension && a.stones == b.stones &&
           a.usable == b.usable;
  }
};

// What a thrower's client tells the server: Take or Impact.
struct StonePileReport {
  StonePileMessageKind kind = StonePileMessageKind::Take;
  std::uint32_t pile = 0;
  std::array<float, 3> position{};  // Impact

  friend bool operator==(const StonePileReport& a, const StonePileReport& b) {
    return a.kind == b.kind && a.pile == b.pile && a.position == b.position;
  }
};

namespace ladder_stone_detail {

inline constexpr std::uint8_t kUsable = 1;
inline constexpr std::uint8_t kMine = 2;
inline constexpr std::uint8_t kLadderFlags = 3;
inline constexpr std::uint8_t kPileFlags = 1;

inline bool valid_stones(std::int64_t stones) {
  return stones >= -1 && stones <= kMaxPileStones;
}

inline bool valid_ladder_spawn(const SiegeLadderOrder& o) {
  using siege_detail::finite;
  return std::uint8_t(o.ladder) < kLadderKinds && std::isfinite(o.height) && o.height > 0.0f &&
         o.height <= kMaxLadderHeight && std::isfinite(o.lean) &&
         std::fabs(o.lean) <= kMaxLadderLean && finite(o.position) && finite(o.rotation);
}

inline void write_ladder_state(core::BitWriter& w, const SiegeLadderOrder& o) {
  w.write_bits(std::uint8_t(o.pose), 8);
  std::uint8_t flags = 0;
  if (o.usable) {
    flags |= kUsable;
  }
  if (o.mine) {
    flags |= kMine;
  }
  w.write_bits(flags, 8);
}

inline bool read_ladder_state(core::BitReader& r, SiegeLadderOrder& o) {
  const std::uint64_t pose = r.read_bits(8);
  const std::uint64_t flags = r.read_bits(8);
  if (r.overrun() || pose >= kLadderPoses || (flags & ~std::uint64_t(kLadderFlags)) != 0) {
    return false;
  }
  o.pose = LadderPose(pose);
  o.usable = (flags & kUsable) != 0;
  o.mine = (flags & kMine) != 0;
  return true;
}

inline void write_pile_state(core::BitWriter& w, const StonePileOrder& o) {
  w.write_varuint(std::uint64_t(o.stones + 1));
  w.write_bits(o.usable ? kUsable : 0, 8);
}

inline bool read_pile_state(core::BitReader& r, StonePileOrder& o) {
  const std::uint64_t stones = r.read_varuint();
  const std::uint64_t flags = r.read_bits(8);
  if (r.overrun() || stones > std::uint64_t(kMaxPileStones) + 1 ||
      (flags & ~std::uint64_t(kPileFlags)) != 0) {
    return false;
  }
  o.stones = std::int64_t(stones) - 1;
  o.usable = (flags & kUsable) != 0;
  return true;
}

inline bool read_world(core::BitReader& r, std::uint32_t& world) {
  const std::uint64_t value = r.read_varuint();
  if (r.overrun() || value > 0xffffffffull) {
    return false;
  }
  world = std::uint32_t(value);
  return true;
}

}  // namespace ladder_stone_detail

// Empty when the order cannot travel.
inline std::vector<std::uint8_t> encode_siege_ladder(const SiegeLadderOrder& order) {
  using namespace siege_detail;
  using namespace ladder_stone_detail;
  core::BitWriter w;
  w.write_bits(kSiegeLadderTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  if (order.kind == LadderMessageKind::Refused) {
    return w.bytes();
  }
  if (order.id == 0) {
    return {};
  }
  w.write_varuint(order.id);
  switch (order.kind) {
    case LadderMessageKind::Spawn:
      if (!valid_ladder_spawn(order) || std::uint8_t(order.pose) >= kLadderPoses) {
        return {};
      }
      w.write_bits(std::uint8_t(order.ladder), 8);
      write_float(w, order.height);
      write_float(w, order.lean);
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      w.write_varuint(order.dimension);
      write_ladder_state(w, order);
      break;
    case LadderMessageKind::Pose:
      if (std::uint8_t(order.pose) >= kLadderPoses) {
        return {};
      }
      write_ladder_state(w, order);
      break;
    case LadderMessageKind::Destroy:
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_siege_ladder(const std::vector<std::uint8_t>& p, SiegeLadderOrder& order) {
  using namespace siege_detail;
  using namespace ladder_stone_detail;
  if (p.size() < 2 || p[0] != kSiegeLadderTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  SiegeLadderOrder decoded;
  decoded.kind = LadderMessageKind(r.read_bits(8));
  if (decoded.kind == LadderMessageKind::Refused) {
    if (!at_end(r)) {
      return false;
    }
    order = decoded;
    return true;
  }
  if (!read_id(r, decoded.id)) {
    return false;
  }
  switch (decoded.kind) {
    case LadderMessageKind::Spawn: {
      decoded.ladder = LadderKind(r.read_bits(8));
      decoded.height = read_float(r);
      decoded.lean = read_float(r);
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      if (r.overrun() || !valid_ladder_spawn(decoded) || !read_world(r, decoded.dimension) ||
          !read_ladder_state(r, decoded)) {
        return false;
      }
      break;
    }
    case LadderMessageKind::Pose:
      if (!read_ladder_state(r, decoded)) {
        return false;
      }
      break;
    case LadderMessageKind::Destroy:
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

inline std::vector<std::uint8_t> encode_siege_ladder_report(const SiegeLadderReport& report) {
  if (report.ladder == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kSiegeLadderTag, 8);
  w.write_bits(std::uint8_t(report.kind), 8);
  w.write_varuint(report.ladder);
  switch (report.kind) {
    case LadderMessageKind::Use:
      if (std::uint8_t(report.action) >= kLadderActions) {
        return {};
      }
      w.write_bits(std::uint8_t(report.action), 8);
      break;
    case LadderMessageKind::Settled:
      if (report.pose != LadderPose::Down && report.pose != LadderPose::Standing) {
        return {};
      }
      w.write_bits(std::uint8_t(report.pose), 8);
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_siege_ladder_report(const std::vector<std::uint8_t>& p,
                                       SiegeLadderReport& report) {
  using namespace siege_detail;
  if (p.size() < 3 || p[0] != kSiegeLadderTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  SiegeLadderReport decoded;
  decoded.kind = LadderMessageKind(r.read_bits(8));
  if (!read_id(r, decoded.ladder)) {
    return false;
  }
  const std::uint64_t value = r.read_bits(8);
  if (!at_end(r)) {
    return false;
  }
  switch (decoded.kind) {
    case LadderMessageKind::Use:
      if (value >= kLadderActions) {
        return false;
      }
      decoded.action = LadderAction(value);
      break;
    case LadderMessageKind::Settled:
      decoded.pose = LadderPose(value);
      if (decoded.pose != LadderPose::Down && decoded.pose != LadderPose::Standing) {
        return false;
      }
      break;
    default:
      return false;
  }
  report = decoded;
  return true;
}

inline std::vector<std::uint8_t> encode_stone_pile(const StonePileOrder& order) {
  using namespace siege_detail;
  using namespace ladder_stone_detail;
  if (order.id == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kStonePileTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.id);
  switch (order.kind) {
    case StonePileMessageKind::Spawn:
      if (!finite(order.position) || !finite(order.rotation) || !valid_stones(order.stones)) {
        return {};
      }
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      w.write_varuint(order.dimension);
      write_pile_state(w, order);
      break;
    case StonePileMessageKind::Update:
      if (!valid_stones(order.stones)) {
        return {};
      }
      write_pile_state(w, order);
      break;
    case StonePileMessageKind::Destroy:
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_stone_pile(const std::vector<std::uint8_t>& p, StonePileOrder& order) {
  using namespace siege_detail;
  using namespace ladder_stone_detail;
  if (p.size() < 3 || p[0] != kStonePileTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  StonePileOrder decoded;
  decoded.kind = StonePileMessageKind(r.read_bits(8));
  if (!read_id(r, decoded.id)) {
    return false;
  }
  switch (decoded.kind) {
    case StonePileMessageKind::Spawn:
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      if (r.overrun() || !finite(decoded.position) || !finite(decoded.rotation) ||
          !read_world(r, decoded.dimension) || !read_pile_state(r, decoded)) {
        return false;
      }
      break;
    case StonePileMessageKind::Update:
      if (!read_pile_state(r, decoded)) {
        return false;
      }
      break;
    case StonePileMessageKind::Destroy:
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

inline std::vector<std::uint8_t> encode_stone_pile_report(const StonePileReport& report) {
  using namespace siege_detail;
  if (report.pile == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kStonePileTag, 8);
  w.write_bits(std::uint8_t(report.kind), 8);
  w.write_varuint(report.pile);
  switch (report.kind) {
    case StonePileMessageKind::Take:
      break;
    case StonePileMessageKind::Impact:
      if (!finite(report.position)) {
        return {};
      }
      write_floats(w, report.position);
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_stone_pile_report(const std::vector<std::uint8_t>& p, StonePileReport& report) {
  using namespace siege_detail;
  if (p.size() < 3 || p[0] != kStonePileTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  StonePileReport decoded;
  decoded.kind = StonePileMessageKind(r.read_bits(8));
  if (!read_id(r, decoded.pile)) {
    return false;
  }
  switch (decoded.kind) {
    case StonePileMessageKind::Take:
      break;
    case StonePileMessageKind::Impact:
      read_floats(r, decoded.position);
      if (r.overrun() || !finite(decoded.position)) {
        return false;
      }
      break;
    default:
      return false;
  }
  if (!at_end(r)) {
    return false;
  }
  report = decoded;
  return true;
}

}  // namespace kcd2s::integration
