#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "kcd2s/core/bits.hpp"
#include "kcd2s/integration/placed_objects.hpp"

// Point lights the server places: Light.spawn. The server holds
// them; every ready client shows them in its game, a client whose world becomes ready later too.
//
//   LIGHT : server -> a ready player's client, on the control channel (0):
//           [tag][u8 kind][varuint entity id] then by kind
//             Spawn, Update : [3 floats position][3 floats color][float intensity][float radius]
//                             [u8 cast shadows][varuint body][text joint]   (an update replaces
//                             the light as a whole; body is a player's or NPC's network id the
//                             light hangs on, 0 for none, and position is then the offset from it
//                             or from its joint. An order without the last two is read
//                             as one without a body.)
//             Destroy       : nothing
//
// Client/runtime/lights/lights.hpp applies them.
namespace kcd2s::integration {

inline constexpr std::uint8_t kLightTag = 153;

// The bounds of a light, as Light.spawn checks them.
inline constexpr float kLightIntensityMax = 100.0f;
inline constexpr float kLightRadiusMin = 0.1f;
inline constexpr float kLightRadiusMax = 100.0f;
// A light's position, at most this far from the world's origin on any axis.
inline constexpr float kLightPositionMax = 100000.0f;
// Orders a client holds before its game takes them (further ones are dropped).
inline constexpr std::size_t kMaxLightOrders = 512;
// A light on a body: at most this far from it, and its joint's name at most this long.
inline constexpr float kLightOffsetMax = 8.0f;
inline constexpr std::size_t kMaxLightJointBytes = 64;

enum class LightKind : std::uint8_t {
  Spawn = 1,
  Update = 2,
  Destroy = 3,
};

struct LightOrder {
  LightKind kind = LightKind::Spawn;
  std::uint32_t id = 0;
  std::array<float, 3> position{};
  std::array<float, 3> color{1.0f, 0.6f, 0.3f};
  float intensity = 1.0f;
  float radius = 5.0f;
  bool cast_shadows = false;
  // The body it hangs on (a network id, 0 for none) and the joint of it ("" for the body).
  std::uint32_t body = 0;
  std::string joint;

  friend bool operator==(const LightOrder& a, const LightOrder& b) {
    return a.kind == b.kind && a.id == b.id && a.position == b.position && a.color == b.color &&
           a.intensity == b.intensity && a.radius == b.radius && a.cast_shadows == b.cast_shadows &&
           a.body == b.body && a.joint == b.joint;
  }
};

// Whether a spawn or update can travel: a position within the world, colour channels from 0 to
// 1, intensity from 0 to 100 and radius from 0.1 to 100 metres.
inline bool light_valid(const LightOrder& order) {
  if (order.id == 0) {
    return false;
  }
  if (order.kind == LightKind::Destroy) {
    return true;
  }
  if (order.kind != LightKind::Spawn && order.kind != LightKind::Update) {
    return false;
  }
  const float position_max = order.body != 0 ? kLightOffsetMax : kLightPositionMax;
  for (const float value : order.position) {
    if (!std::isfinite(value) || std::fabs(value) > position_max) {
      return false;
    }
  }
  if (!order.joint.empty() &&
      (order.body == 0 || !placed_detail::printable(order.joint, kMaxLightJointBytes))) {
    return false;
  }
  for (const float value : order.color) {
    if (!std::isfinite(value) || value < 0.0f || value > 1.0f) {
      return false;
    }
  }
  return std::isfinite(order.intensity) && order.intensity >= 0.0f &&
         order.intensity <= kLightIntensityMax && std::isfinite(order.radius) &&
         order.radius >= kLightRadiusMin && order.radius <= kLightRadiusMax;
}

// Empty when the order cannot travel.
inline std::vector<std::uint8_t> encode_light(const LightOrder& order) {
  using namespace placed_detail;
  if (!light_valid(order)) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kLightTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.id);
  if (order.kind != LightKind::Destroy) {
    write_floats(w, order.position);
    write_floats(w, order.color);
    write_float(w, order.intensity);
    write_float(w, order.radius);
    w.write_bits(order.cast_shadows ? 1 : 0, 8);
    w.write_varuint(order.body);
    write_text(w, order.joint);
  }
  return w.bytes();
}

inline bool decode_light(const std::vector<std::uint8_t>& p, LightOrder& order) {
  using namespace placed_detail;
  if (p.size() < 3 || p[0] != kLightTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  LightOrder decoded;
  decoded.kind = LightKind(r.read_bits(8));
  const std::uint64_t id = r.read_varuint();
  if (r.overrun() || id > 0xFFFFFFFFull) {
    return false;
  }
  decoded.id = std::uint32_t(id);
  if (decoded.kind == LightKind::Spawn || decoded.kind == LightKind::Update) {
    read_floats(r, decoded.position);
    read_floats(r, decoded.color);
    decoded.intensity = read_float(r);
    decoded.radius = read_float(r);
    const std::uint64_t shadows = r.read_bits(8);
    if (shadows > 1) {
      return false;
    }
    decoded.cast_shadows = shadows == 1;
    // An order from a server before ends here: no body.
    if (r.bits_remaining() >= 8) {
      const std::uint64_t body = r.read_varuint();
      if (body > 0xFFFFFFFFull || !read_text(r, kMaxLightJointBytes, decoded.joint)) {
        return false;
      }
      decoded.body = std::uint32_t(body);
    }
  }
  if (r.overrun() || r.bits_remaining() != 0 || !light_valid(decoded)) {
    return false;
  }
  order = decoded;
  return true;
}

}  // namespace kcd2s::integration
