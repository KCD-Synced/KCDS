#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/placed_objects.hpp"

// Gizmos: debug shapes the server draws in one player's or every
// player's world for a while, with the game's renderer aux geometry.
//
//   GIZMO : server -> a ready player's client, on the control channel (0):
//           [tag][u8 kind] then by kind
//             Draw     : [varuint gizmo][u8 shape][3 floats position][3 floats extent]
//                        [4 floats rotation][float size][4 floats colour r, g, b, a]
//                        [varuint duration ms][text]
//             Clear    : [varuint gizmo]
//             ClearAll : nothing
//           `extent` is a line's end point or a box's half sizes; `size` a sphere's radius or the
//           text's size; `rotation` turns a box; `text` is empty but for a text. A duration of
//           0 keeps the gizmo until it is cleared; a Draw with the id of a drawn one replaces it.
//
// Client/runtime/gizmos/gizmos.hpp draws them.
namespace kcd2s::integration {

inline constexpr std::uint8_t kGizmoTag = 103;

inline constexpr std::size_t kMaxGizmoTextBytes = 128;
inline constexpr std::uint32_t kMaxGizmoMilliseconds = 3600000;
// Gizmos a client keeps drawn at once (further draws are dropped).
inline constexpr std::size_t kMaxGizmos = 1024;
inline constexpr std::size_t kMaxGizmoOrders = 512;

enum class GizmoKind : std::uint8_t {
  Draw = 1,
  Clear = 2,
  ClearAll = 3,
};

enum class GizmoShape : std::uint8_t {
  Line = 1,
  Sphere = 2,
  Box = 3,
  Text = 4,
};

struct GizmoOrder {
  GizmoKind kind = GizmoKind::Draw;
  std::uint32_t gizmo = 0;
  GizmoShape shape = GizmoShape::Line;
  std::array<float, 3> position{};
  std::array<float, 3> extent{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  float size = 1.0f;
  std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
  std::uint32_t duration_ms = 0;
  std::string text;

  friend bool operator==(const GizmoOrder& a, const GizmoOrder& b) {
    return a.kind == b.kind && a.gizmo == b.gizmo && a.shape == b.shape &&
           a.position == b.position && a.extent == b.extent && a.rotation == b.rotation &&
           a.size == b.size && a.color == b.color && a.duration_ms == b.duration_ms &&
           a.text == b.text;
  }
};

namespace gizmo_detail {

inline bool printable_or_empty(const std::string& text) {
  return text.empty() || placed_detail::printable(text, kMaxGizmoTextBytes);
}

inline bool valid_draw(const GizmoOrder& order) {
  bool color_ok = placed_detail::finite(order.color);
  for (const float channel : order.color) {
    color_ok = color_ok && channel >= 0.0f && channel <= 1.0f;
  }
  const bool shape_ok = order.shape >= GizmoShape::Line && order.shape <= GizmoShape::Text;
  const bool text_ok = order.shape == GizmoShape::Text ? !order.text.empty() : order.text.empty();
  return order.gizmo != 0 && shape_ok && color_ok && placed_detail::finite(order.position) &&
         placed_detail::finite(order.extent) && placed_detail::finite(order.rotation) &&
         std::isfinite(order.size) && order.size >= 0.0f && order.size <= 1000.0f &&
         order.duration_ms <= kMaxGizmoMilliseconds && printable_or_empty(order.text) && text_ok;
}

}  // namespace gizmo_detail

// Empty when the order cannot travel.
inline std::vector<std::uint8_t> encode_gizmo(const GizmoOrder& order) {
  core::BitWriter w;
  w.write_bits(kGizmoTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  switch (order.kind) {
    case GizmoKind::Draw:
      if (!gizmo_detail::valid_draw(order)) {
        return {};
      }
      w.write_varuint(order.gizmo);
      w.write_bits(std::uint8_t(order.shape), 8);
      placed_detail::write_floats(w, order.position);
      placed_detail::write_floats(w, order.extent);
      placed_detail::write_floats(w, order.rotation);
      placed_detail::write_float(w, order.size);
      placed_detail::write_floats(w, order.color);
      w.write_varuint(order.duration_ms);
      placed_detail::write_text(w, order.text);
      break;
    case GizmoKind::Clear:
      if (order.gizmo == 0) {
        return {};
      }
      w.write_varuint(order.gizmo);
      break;
    case GizmoKind::ClearAll:
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_gizmo(const std::vector<std::uint8_t>& p, GizmoOrder& order) {
  if (p.size() < 2 || p[0] != kGizmoTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  GizmoOrder decoded;
  decoded.kind = GizmoKind(r.read_bits(8));
  const auto read_id = [&]() {
    const std::uint64_t id = r.read_varuint();
    decoded.gizmo = std::uint32_t(id);
    return !r.overrun() && id != 0 && id <= 0xffffffffull;
  };
  switch (decoded.kind) {
    case GizmoKind::Draw: {
      if (!read_id()) {
        return false;
      }
      decoded.shape = GizmoShape(r.read_bits(8));
      placed_detail::read_floats(r, decoded.position);
      placed_detail::read_floats(r, decoded.extent);
      placed_detail::read_floats(r, decoded.rotation);
      decoded.size = placed_detail::read_float(r);
      placed_detail::read_floats(r, decoded.color);
      const std::uint64_t ms = r.read_varuint();
      if (ms > kMaxGizmoMilliseconds) {
        return false;
      }
      decoded.duration_ms = std::uint32_t(ms);
      if (!placed_detail::read_text(r, kMaxGizmoTextBytes, decoded.text) ||
          !gizmo_detail::valid_draw(decoded)) {
        return false;
      }
      break;
    }
    case GizmoKind::Clear:
      if (!read_id()) {
        return false;
      }
      break;
    case GizmoKind::ClearAll:
      break;
    default:
      return false;
  }
  if (r.overrun()) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

}  // namespace kcd2s::integration
