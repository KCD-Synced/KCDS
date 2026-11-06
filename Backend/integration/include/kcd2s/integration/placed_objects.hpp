#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "kcd2s/core/bits.hpp"

// Props and placed effects: the server holds them, every
// client in their dimension shows them in its game.
//
//   PROP : server -> a ready player's client, on the control channel (0):
//          [tag][u8 kind][varuint entity id] then by kind
//            Spawn   : [text model][3 floats position][4 floats rotation][3 floats scale]
//                      [u8 physics][varuint dimension][float mass][varuint body][text joint]
//                      (body: a player's or NPC's network id the prop hangs on, 0 for none,
//                      with no physics, position and rotation then its offset from the body or
//                      its joint; an order without the last two has no body)
//            Update  : [3 floats scale][u8 physics][float mass]   (the client rebuilds the prop)
//            Destroy : nothing
//            Move    : [3 floats position][4 floats rotation]   (the client moves the prop as it
//                      is; a rigid one it simulates comes to rest there)
//                      [varuint glide ms][float arc]   (optional, Prop.glideTo: the
//                      client glides it there over the ms, lifted on an arc that many metres
//                      high at the middle; an order without them moves it at once)
//            Simulate: [u8 simulate]   (a rigid prop's body: this client simulates it and
//                      reports it, or follows the poses the server passes on)
//            Push    : [u8 push][3 floats linear][3 floats angular][u8 has point]
//                      [3 floats point]   (to the client simulating a rigid prop: an impulse, a
//                      velocity, wake or sleep)
//   PROP_REPORT : the client simulating a rigid prop -> server, on the control channel (0):
//          [tag][varuint entity id][3 floats position][4 floats rotation][u8 resting]
//          [float hit speed]
//          (where the body is while it moves, and once where it came to rest; the hit speed, m/s,
//          when it just struck something, 0 otherwise: a report without it has
//          none)
//   VFX  : server -> a ready player's client, on the control channel (0):
//          [tag][u8 kind] then by kind
//            Spawn   : [varuint entity id][text effect][3 floats position][4 floats rotation]
//                      [varuint dimension][tuning][u8 prime][varuint body][text joint]
//                      (the player or NPC it hangs on, 0 for none, and the joint, empty for the
//                      body itself; the position is then the offset from it. An
//                      order without the last two hangs on nothing.)
//            Update  : [varuint entity id][tuning]   (the client rebuilds the emitter)
//            Destroy : [varuint entity id]
//            Burst   : [text effect][3 floats position][4 floats rotation][varuint dimension]
//                      [varuint duration ms][tuning][u8 prime]
//            BurstOn : [text effect][varuint entity id to follow][3 floats offset]
//                      [4 floats rotation][varuint duration ms][tuning][u8 prime][text joint]
//                      (the joint of the body it hangs on, empty for the body itself; issue
// An order without it is read as one without a joint.)
//          The tuning is six floats: scale, count scale, speed scale, time scale, strength and
//          pulse period. A burst ends on the client's own timer; there is no end message.
//
// Client/runtime/props/placed_props.hpp and Client/runtime/vfx/placed_effects.hpp apply them.
namespace kcd2s::integration {

// 69 and 70 are the world time and weather messages, 71 to 75 the doors'.
inline constexpr std::uint8_t kPropTag = 76;
inline constexpr std::uint8_t kVfxTag = 77;
inline constexpr std::uint8_t kPropReportTag = 154;

inline constexpr std::size_t kMaxPlacedModelBytes = 255;
inline constexpr std::size_t kMaxPlacedEffectBytes = 128;
// A body joint's name a burst hangs on.
inline constexpr std::size_t kMaxVfxJointBytes = 64;
// A prop on a body: its joint's name at most this long, at most this far from it.
inline constexpr std::size_t kMaxPropJointBytes = 64;
inline constexpr float kMaxPropOffset = 5.0f;
// A move's glide (Prop.glideTo): at most this long, its arc at most this high.
inline constexpr std::uint32_t kMaxPropGlideMs = 10000;
inline constexpr float kMaxPropGlideArc = 5.0f;
inline constexpr std::uint32_t kMaxBurstMilliseconds = 60000;
// Orders a client holds before its game takes them (further ones are dropped, and counted): as
// many as the server can have entities at once, so a town that arrives in one frame (a player who
// joins into it, or a script that builds it) is never cut. The game takes them all every frame.
// Was 512: a burst cut the rest off silently (a bakery of 497 objects showed 465).
inline constexpr std::size_t kMaxPlacedOrders = 0xFFFF;

enum class PropKind : std::uint8_t {
  Spawn = 1,
  Update = 2,
  Destroy = 3,
  Move = 4,
  Simulate = 5,
  Push = 6,
};

// What a Push does to a rigid prop's body.
enum class PropPush : std::uint8_t {
  Impulse = 1,   // linear: kg m/s, angular: kg m^2/s, at the point when there is one
  Velocity = 2,  // linear: m/s, angular: rad/s
  Wake = 3,
  Sleep = 4,
};

enum class PropPhysics : std::uint8_t {
  None = 0,
  Static = 1,
  Rigid = 2,
  // A static body that is not drawn: a piece of an area's collision walls.
  Wall = 3,
};

struct PropOrder {
  PropKind kind = PropKind::Spawn;
  std::uint32_t id = 0;
  std::string model;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> scale{1.0f, 1.0f, 1.0f};
  PropPhysics physics = PropPhysics::Static;
  std::uint32_t dimension = 0;
  // A rigid prop's mass in kilograms; 0 takes the mass the mesh's collision gives it.
  float mass = 0.0f;
  // Simulate: whether this client simulates the body.
  bool simulate = false;
  // Push.
  PropPush push = PropPush::Wake;
  std::array<float, 3> linear{};
  std::array<float, 3> angular{};
  bool has_point = false;
  std::array<float, 3> point{};
  // Spawn: the body it hangs on (a network id, 0 for none) and the joint of it ("" for the body),
  std::uint32_t body = 0;
  std::string joint;
  // Move: glide there over this many ms (0 at once) on an arc this many metres high.
  std::uint32_t glide_ms = 0;
  float glide_arc = 0.0f;

  friend bool operator==(const PropOrder& a, const PropOrder& b) {
    return a.kind == b.kind && a.id == b.id && a.model == b.model && a.position == b.position &&
           a.rotation == b.rotation && a.scale == b.scale && a.physics == b.physics &&
           a.dimension == b.dimension && a.mass == b.mass && a.simulate == b.simulate &&
           a.push == b.push && a.linear == b.linear && a.angular == b.angular &&
           a.has_point == b.has_point && a.point == b.point && a.body == b.body &&
           a.joint == b.joint && a.glide_ms == b.glide_ms && a.glide_arc == b.glide_arc;
  }
};

// Where the body of a rigid prop the client simulates is.
struct PropReport {
  std::uint32_t id = 0;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  bool resting = false;
  // The speed it struck something with (m/s), 0 for none.
  float hit_speed = 0.0f;

  friend bool operator==(const PropReport& a, const PropReport& b) {
    return a.id == b.id && a.position == b.position && a.rotation == b.rotation &&
           a.resting == b.resting && a.hit_speed == b.hit_speed;
  }
};

// A hit faster than this (m/s) is no rigid prop's.
inline constexpr float kMaxPropHitSpeed = 1000.0f;

enum class VfxKind : std::uint8_t {
  Spawn = 1,
  Update = 2,
  Destroy = 3,
  Burst = 4,
  BurstOn = 5,
};

struct VfxTuning {
  float scale = 1.0f;
  float count_scale = 1.0f;
  float speed_scale = 1.0f;
  float time_scale = 1.0f;
  float strength = -1.0f;  // -1 to 1; -1 is the effect's own
  float pulse_period = 0.0f;

  friend bool operator==(const VfxTuning& a, const VfxTuning& b) {
    return a.scale == b.scale && a.count_scale == b.count_scale && a.speed_scale == b.speed_scale &&
           a.time_scale == b.time_scale && a.strength == b.strength &&
           a.pulse_period == b.pulse_period;
  }
};

struct VfxOrder {
  VfxKind kind = VfxKind::Spawn;
  std::uint32_t id = 0;  // the effect (spawn, update, destroy) or the entity a burst follows
  std::string effect;
  std::array<float, 3> position{};  // BurstOn: the offset from the entity
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  std::uint32_t dimension = 0;
  std::uint32_t duration_ms = 0;
  VfxTuning tuning;
  bool prime = false;
  std::string joint;  // BurstOn, Spawn: the body's joint it hangs on, empty for the body
  // Spawn: the player or NPC (network id) it hangs on, 0 for none.
  std::uint32_t body = 0;

  friend bool operator==(const VfxOrder& a, const VfxOrder& b) {
    return a.kind == b.kind && a.id == b.id && a.effect == b.effect && a.position == b.position &&
           a.rotation == b.rotation && a.dimension == b.dimension &&
           a.duration_ms == b.duration_ms && a.tuning == b.tuning && a.prime == b.prime &&
           a.joint == b.joint && a.body == b.body;
  }
};

namespace placed_detail {

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

inline void write_text(core::BitWriter& w, const std::string& text) {
  w.write_varuint(text.size());
  for (const char c : text) {
    w.write_bits(std::uint8_t(c), 8);
  }
}

inline bool read_text(core::BitReader& r, std::size_t max_bytes, std::string& text) {
  const std::uint64_t size = r.read_varuint();
  if (r.overrun() || size > max_bytes) {
    return false;
  }
  text.clear();
  for (std::uint64_t i = 0; i < size; ++i) {
    text.push_back(char(r.read_bits(8)));
  }
  return !r.overrun();
}

// Printable ASCII only: model paths and effect names.
inline bool printable(const std::string& text, std::size_t max_bytes) {
  if (text.empty() || text.size() > max_bytes) {
    return false;
  }
  for (const char c : text) {
    if (c < 0x20 || c > 0x7e) {
      return false;
    }
  }
  return true;
}

// A rigid prop's mass: 0 (the mesh's own) to 100,000 kg.
inline bool valid_mass(float mass) {
  return std::isfinite(mass) && mass >= 0.0f && mass <= 100000.0f;
}

inline bool valid_push(PropPush push) {
  return push >= PropPush::Impulse && push <= PropPush::Sleep;
}

// A prop on a body: no physics, a joint only with a body, the offset within reach.
inline bool valid_hanging(const PropOrder& order) {
  if (order.body == 0) {
    return order.joint.empty();
  }
  if (order.physics != PropPhysics::None) {
    return false;
  }
  if (!order.joint.empty() && !printable(order.joint, kMaxPropJointBytes)) {
    return false;
  }
  for (const float value : order.position) {
    if (std::fabs(value) > kMaxPropOffset) {
      return false;
    }
  }
  return true;
}

inline bool valid_tuning(const VfxTuning& t) {
  return std::isfinite(t.scale) && t.scale > 0.0f && std::isfinite(t.count_scale) &&
         t.count_scale >= 0.0f && std::isfinite(t.speed_scale) && t.speed_scale >= 0.0f &&
         std::isfinite(t.time_scale) && t.time_scale >= 0.0f && std::isfinite(t.strength) &&
         t.strength >= -1.0f && t.strength <= 1.0f && std::isfinite(t.pulse_period) &&
         t.pulse_period >= 0.0f;
}

inline void write_tuning(core::BitWriter& w, const VfxTuning& t) {
  write_float(w, t.scale);
  write_float(w, t.count_scale);
  write_float(w, t.speed_scale);
  write_float(w, t.time_scale);
  write_float(w, t.strength);
  write_float(w, t.pulse_period);
}

inline void read_tuning(core::BitReader& r, VfxTuning& t) {
  t.scale = read_float(r);
  t.count_scale = read_float(r);
  t.speed_scale = read_float(r);
  t.time_scale = read_float(r);
  t.strength = read_float(r);
  t.pulse_period = read_float(r);
}

}  // namespace placed_detail

// Empty when the order cannot travel.
inline std::vector<std::uint8_t> encode_prop(const PropOrder& order) {
  using namespace placed_detail;
  core::BitWriter w;
  w.write_bits(kPropTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.id);
  switch (order.kind) {
    case PropKind::Spawn:
      if (!printable(order.model, kMaxPlacedModelBytes) || !finite(order.position) ||
          !finite(order.rotation) || !finite(order.scale)) {
        return {};
      }
      write_text(w, order.model);
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      write_floats(w, order.scale);
      w.write_bits(std::uint8_t(order.physics), 8);
      w.write_varuint(order.dimension);
      if (!valid_mass(order.mass) || !valid_hanging(order)) {
        return {};
      }
      write_float(w, order.mass);
      w.write_varuint(order.body);
      write_text(w, order.joint);
      break;
    case PropKind::Update:
      if (!finite(order.scale) || !valid_mass(order.mass)) {
        return {};
      }
      write_floats(w, order.scale);
      w.write_bits(std::uint8_t(order.physics), 8);
      write_float(w, order.mass);
      break;
    case PropKind::Destroy:
      break;
    case PropKind::Move:
      if (!finite(order.position) || !finite(order.rotation)) {
        return {};
      }
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      if (order.glide_ms > 0) {
        if (order.glide_ms > kMaxPropGlideMs || !(std::fabs(order.glide_arc) <= kMaxPropGlideArc)) {
          return {};
        }
        w.write_varuint(order.glide_ms);
        write_float(w, order.glide_arc);
      }
      break;
    case PropKind::Simulate:
      w.write_bits(order.simulate ? 1u : 0u, 8);
      break;
    case PropKind::Push:
      if (!valid_push(order.push) || !finite(order.linear) || !finite(order.angular) ||
          !finite(order.point)) {
        return {};
      }
      w.write_bits(std::uint8_t(order.push), 8);
      write_floats(w, order.linear);
      write_floats(w, order.angular);
      w.write_bits(order.has_point ? 1u : 0u, 8);
      write_floats(w, order.point);
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_prop(const std::vector<std::uint8_t>& p, PropOrder& order) {
  using namespace placed_detail;
  if (p.size() < 3 || p[0] != kPropTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  PropOrder decoded;
  decoded.kind = PropKind(r.read_bits(8));
  const std::uint64_t id = r.read_varuint();
  if (r.overrun() || id == 0 || id > 0xffffffffull) {
    return false;
  }
  decoded.id = std::uint32_t(id);
  switch (decoded.kind) {
    case PropKind::Spawn: {
      if (!read_text(r, kMaxPlacedModelBytes, decoded.model)) {
        return false;
      }
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      read_floats(r, decoded.scale);
      decoded.physics = PropPhysics(r.read_bits(8));
      const std::uint64_t world = r.read_varuint();
      if (world > 0xffffffffull) {
        return false;
      }
      decoded.dimension = std::uint32_t(world);
      decoded.mass = read_float(r);
      // An order from a server before ends here: no body.
      if (r.bits_remaining() >= 8) {
        const std::uint64_t body = r.read_varuint();
        if (body > 0xffffffffull || !read_text(r, kMaxPropJointBytes, decoded.joint)) {
          return false;
        }
        decoded.body = std::uint32_t(body);
      }
      if (!valid_hanging(decoded)) {
        return false;
      }
      break;
    }
    case PropKind::Update:
      read_floats(r, decoded.scale);
      decoded.physics = PropPhysics(r.read_bits(8));
      decoded.mass = read_float(r);
      break;
    case PropKind::Destroy:
      break;
    case PropKind::Move:
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      // An order from a server before ends here: it moves at once.
      if (r.bits_remaining() >= 8) {
        const std::uint64_t ms = r.read_varuint();
        decoded.glide_arc = read_float(r);
        if (ms == 0 || ms > kMaxPropGlideMs ||
            !(std::fabs(decoded.glide_arc) <= kMaxPropGlideArc)) {
          return false;
        }
        decoded.glide_ms = std::uint32_t(ms);
      }
      break;
    case PropKind::Simulate:
      decoded.simulate = r.read_bits(8) != 0;
      break;
    case PropKind::Push:
      decoded.push = PropPush(r.read_bits(8));
      read_floats(r, decoded.linear);
      read_floats(r, decoded.angular);
      decoded.has_point = r.read_bits(8) != 0;
      read_floats(r, decoded.point);
      if (!valid_push(decoded.push)) {
        return false;
      }
      break;
    default:
      return false;
  }
  if (r.overrun() || decoded.physics > PropPhysics::Wall || !finite(decoded.position) ||
      !finite(decoded.rotation) || !finite(decoded.scale) || !valid_mass(decoded.mass) ||
      !finite(decoded.linear) || !finite(decoded.angular) || !finite(decoded.point) ||
      (decoded.kind == PropKind::Spawn && !printable(decoded.model, kMaxPlacedModelBytes))) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

// Empty when the report cannot travel.
inline std::vector<std::uint8_t> encode_prop_report(const PropReport& report) {
  using namespace placed_detail;
  if (report.id == 0 || !finite(report.position) || !finite(report.rotation) ||
      !std::isfinite(report.hit_speed) || report.hit_speed < 0.0f ||
      report.hit_speed > kMaxPropHitSpeed) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPropReportTag, 8);
  w.write_varuint(report.id);
  write_floats(w, report.position);
  write_floats(w, report.rotation);
  w.write_bits(report.resting ? 1u : 0u, 8);
  write_float(w, report.hit_speed);
  return w.bytes();
}

inline bool decode_prop_report(const std::vector<std::uint8_t>& p, PropReport& report) {
  using namespace placed_detail;
  if (p.size() < 3 || p[0] != kPropReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  PropReport decoded;
  const std::uint64_t id = r.read_varuint();
  if (r.overrun() || id == 0 || id > 0xffffffffull) {
    return false;
  }
  decoded.id = std::uint32_t(id);
  read_floats(r, decoded.position);
  read_floats(r, decoded.rotation);
  decoded.resting = r.read_bits(8) != 0;
  // A report from a client before ends here: no hit.
  if (r.bits_remaining() >= 32) {
    decoded.hit_speed = read_float(r);
  }
  if (r.overrun() || !finite(decoded.position) || !finite(decoded.rotation) ||
      !std::isfinite(decoded.hit_speed) || decoded.hit_speed < 0.0f ||
      decoded.hit_speed > kMaxPropHitSpeed) {
    return false;
  }
  report = decoded;
  return true;
}

inline std::vector<std::uint8_t> encode_vfx(const VfxOrder& order) {
  using namespace placed_detail;
  const bool has_effect = order.kind == VfxKind::Spawn || order.kind == VfxKind::Burst ||
                          order.kind == VfxKind::BurstOn;
  if (has_effect && !printable(order.effect, kMaxPlacedEffectBytes)) {
    return {};
  }
  if (!order.joint.empty() && !printable(order.joint, kMaxVfxJointBytes)) {
    return {};
  }
  if (!valid_tuning(order.tuning) || !finite(order.position) || !finite(order.rotation) ||
      order.duration_ms > kMaxBurstMilliseconds) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kVfxTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  switch (order.kind) {
    case VfxKind::Spawn:
      w.write_varuint(order.id);
      write_text(w, order.effect);
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      w.write_varuint(order.dimension);
      write_tuning(w, order.tuning);
      w.write_bool(order.prime);
      w.write_varuint(order.body);
      write_text(w, order.joint);
      break;
    case VfxKind::Update:
      w.write_varuint(order.id);
      write_tuning(w, order.tuning);
      break;
    case VfxKind::Destroy:
      w.write_varuint(order.id);
      break;
    case VfxKind::Burst:
      write_text(w, order.effect);
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      w.write_varuint(order.dimension);
      w.write_varuint(order.duration_ms);
      write_tuning(w, order.tuning);
      w.write_bool(order.prime);
      break;
    case VfxKind::BurstOn:
      write_text(w, order.effect);
      w.write_varuint(order.id);
      write_floats(w, order.position);
      write_floats(w, order.rotation);
      w.write_varuint(order.duration_ms);
      write_tuning(w, order.tuning);
      w.write_bool(order.prime);
      write_text(w, order.joint);
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_vfx(const std::vector<std::uint8_t>& p, VfxOrder& order) {
  using namespace placed_detail;
  if (p.size() < 3 || p[0] != kVfxTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  VfxOrder decoded;
  decoded.kind = VfxKind(r.read_bits(8));
  const auto read_id = [&]() {
    const std::uint64_t id = r.read_varuint();
    decoded.id = std::uint32_t(id);
    return id != 0 && id <= 0xffffffffull;
  };
  const auto read_world = [&]() {
    const std::uint64_t world = r.read_varuint();
    decoded.dimension = std::uint32_t(world);
    return world <= 0xffffffffull;
  };
  const auto read_duration = [&]() {
    const std::uint64_t ms = r.read_varuint();
    decoded.duration_ms = std::uint32_t(ms);
    return ms <= kMaxBurstMilliseconds;
  };
  bool ok = false;
  switch (decoded.kind) {
    case VfxKind::Spawn:
      ok = read_id() && read_text(r, kMaxPlacedEffectBytes, decoded.effect);
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      ok = ok && read_world();
      read_tuning(r, decoded.tuning);
      decoded.prime = r.read_bool();
      // An order from a server before ends here: on no body.
      if (ok && r.bits_remaining() >= 16) {
        const std::uint64_t body = r.read_varuint();
        decoded.body = std::uint32_t(body);
        ok = body <= 0xffffffffull && read_text(r, kMaxVfxJointBytes, decoded.joint) &&
             (decoded.joint.empty() || (body != 0 && printable(decoded.joint, kMaxVfxJointBytes)));
      }
      break;
    case VfxKind::Update:
      ok = read_id();
      read_tuning(r, decoded.tuning);
      break;
    case VfxKind::Destroy:
      ok = read_id();
      break;
    case VfxKind::Burst:
      ok = read_text(r, kMaxPlacedEffectBytes, decoded.effect);
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      ok = ok && read_world() && read_duration();
      read_tuning(r, decoded.tuning);
      decoded.prime = r.read_bool();
      break;
    case VfxKind::BurstOn:
      ok = read_text(r, kMaxPlacedEffectBytes, decoded.effect) && read_id();
      read_floats(r, decoded.position);
      read_floats(r, decoded.rotation);
      ok = ok && read_duration();
      read_tuning(r, decoded.tuning);
      decoded.prime = r.read_bool();
      // An order from a server before ends here: no joint.
      if (ok && r.bits_remaining() >= 8) {
        ok = read_text(r, kMaxVfxJointBytes, decoded.joint) &&
             (decoded.joint.empty() || printable(decoded.joint, kMaxVfxJointBytes));
      }
      break;
    default:
      return false;
  }
  const bool has_effect = decoded.kind == VfxKind::Spawn || decoded.kind == VfxKind::Burst ||
                          decoded.kind == VfxKind::BurstOn;
  if (!ok || r.overrun() || !valid_tuning(decoded.tuning) || !finite(decoded.position) ||
      !finite(decoded.rotation) ||
      (has_effect && !printable(decoded.effect, kMaxPlacedEffectBytes))) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
