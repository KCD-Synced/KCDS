#pragma once
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/placed_objects.hpp"

// Prop previews: PropGhost and PropPlacer. Both are shown to one
// player only; the server holds them and sends a message only on change.
//
//   GHOST         : server -> one ready player's client, on the control channel (0):
//                   [tag][u8 kind][varuint ghost] then by kind
//                     Create  : [pieces][3 floats position][4 floats rotation][float scale]
//                               [tint][bool hidden]
//                     Pose    : [3 floats position][4 floats rotation][float scale]
//                     Tint    : [tint]
//                     Hidden  : [bool hidden]
//                     Destroy : nothing
//   PLACER        : server -> one ready player's client, on the control channel (0):
//                   [tag][u8 kind][varuint session] then by kind
//                     Begin   : [pieces][float scale][4 floats valid tint][4 floats invalid tint]
//                               [float range][float fallback distance][float vertical offset]
//                               [float yaw (degrees)][u8 flags][float max slope (degrees)]
//                               [float min distance][float max distance]   (a rule < 0: none)
//                     Veto    : [bool vetoed]
//                     End     : nothing
//   PLACER_REPORT : client -> server, on the control channel (0):
//                   [tag][u8 event][varuint session][bool valid] then, for a move or a confirm,
//                   [3 floats position][4 floats rotation][3 floats normal][float distance]
//                   [bool on surface]
//
// The pieces are [u8 count, 1 to 16] and per piece [text model][3 floats offset][4 floats
// rotation]; a tint is [bool present] and then [4 floats r, g, b, a] (0 to 1).
//
// Client/runtime/props/ghosts.hpp and Client/runtime/props/placer.hpp apply them.
namespace kcd2s::integration {

// 100 to 105 are the previews' and the gizmos' (0.2.1); 106 is NPC_LOOK (0.2.0).
inline constexpr std::uint8_t kGhostTag = 100;
inline constexpr std::uint8_t kPlacerTag = 101;
inline constexpr std::uint8_t kPlacerReportTag = 102;

inline constexpr std::size_t kMaxPreviewPieces = 16;
inline constexpr float kMaxPreviewRange = 512.0f;
// Reports a client may send in a second (a move is sent at most ten times a second).
inline constexpr unsigned kMaxPlacerReportsPerSecond = 20;
// Orders a client holds before its game takes them (further ones are dropped).
inline constexpr std::size_t kMaxPreviewOrders = 256;

struct PreviewPiece {
  std::string model;
  std::array<float, 3> offset{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};

  friend bool operator==(const PreviewPiece& a, const PreviewPiece& b) {
    return a.model == b.model && a.offset == b.offset && a.rotation == b.rotation;
  }
};

struct PreviewTint {
  bool present = false;
  std::array<float, 4> rgba{1.0f, 1.0f, 1.0f, 1.0f};

  friend bool operator==(const PreviewTint& a, const PreviewTint& b) {
    return a.present == b.present && (!a.present || a.rgba == b.rgba);
  }
};

enum class GhostKind : std::uint8_t {
  Create = 1,
  Pose = 2,
  Tint = 3,
  Hidden = 4,
  Destroy = 5,
};

struct GhostOrder {
  GhostKind kind = GhostKind::Create;
  std::uint32_t ghost = 0;
  std::vector<PreviewPiece> pieces;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  float scale = 1.0f;
  PreviewTint tint;
  bool hidden = false;
  // A scale per axis, on top of `scale`: travels after the other fields, so a peer
  // without it reads the rest and ignores it.
  std::array<float, 3> axes{1.0f, 1.0f, 1.0f};
  // World Architect's previews: drawn in the game's see-through material instead of
  // their own. Set by the client's own editor only (props/local_previews.cpp); it does not travel.
  bool see_through = false;

  friend bool operator==(const GhostOrder& a, const GhostOrder& b) {
    return a.kind == b.kind && a.ghost == b.ghost && a.pieces == b.pieces &&
           a.position == b.position && a.rotation == b.rotation && a.scale == b.scale &&
           a.tint == b.tint && a.hidden == b.hidden && a.axes == b.axes &&
           a.see_through == b.see_through;
  }
};

// Queues a ghost's order for the client's next frame, at most `max` waiting. A pose
// replaces the ghost's pose still waiting: only the newest is drawn, and poses a script sends every
// frame (a rope between two moving bodies) filled the queue while the game stalled, so a destroy
// behind them was dropped and its ghost stayed on screen. A full queue makes room by dropping a
// waiting pose, never a create, destroy, tint or hide.
inline void queue_ghost_order(std::vector<GhostOrder>& queue, GhostOrder&& order, std::size_t max) {
  const auto is_pose = [](const GhostOrder& queued) { return queued.kind == GhostKind::Pose; };
  if (order.kind == GhostKind::Pose) {
    for (GhostOrder& queued : queue) {
      if (queued.ghost == order.ghost && is_pose(queued)) {
        queued = std::move(order);
        return;
      }
    }
  }
  if (queue.size() >= max) {
    for (auto it = queue.begin(); it != queue.end(); ++it) {
      if (is_pose(*it)) {
        queue.erase(it);
        queue.push_back(std::move(order));
        return;
      }
    }
    return;
  }
  queue.push_back(std::move(order));
}

enum class PlacerKind : std::uint8_t {
  Begin = 1,
  Veto = 2,
  End = 3,
};

// The Begin flags.
enum PlacerFlag : std::uint8_t {
  kPlacerAlignToSurface = 1,
  kPlacerFaceAwayFromPlayer = 2,
  kPlacerKeepOpen = 4,
  kPlacerReportMoves = 8,
  kPlacerRequireSurface = 16,
};

struct PlacerOrder {
  PlacerKind kind = PlacerKind::Begin;
  std::uint32_t session = 0;
  std::vector<PreviewPiece> pieces;
  float scale = 1.0f;
  std::array<float, 4> valid_tint{0.0f, 1.0f, 0.0f, 0.5f};
  std::array<float, 4> invalid_tint{1.0f, 0.0f, 0.0f, 0.5f};
  float range = 10.0f;
  float fallback_distance = 3.0f;
  float vertical_offset = 0.0f;
  float yaw = 0.0f;
  std::uint8_t flags = 0;
  float max_slope = -1.0f;
  float min_distance = -1.0f;
  float max_distance = -1.0f;
  bool vetoed = false;

  friend bool operator==(const PlacerOrder& a, const PlacerOrder& b) {
    return a.kind == b.kind && a.session == b.session && a.pieces == b.pieces &&
           a.scale == b.scale && a.valid_tint == b.valid_tint && a.invalid_tint == b.invalid_tint &&
           a.range == b.range && a.fallback_distance == b.fallback_distance &&
           a.vertical_offset == b.vertical_offset && a.yaw == b.yaw && a.flags == b.flags &&
           a.max_slope == b.max_slope && a.min_distance == b.min_distance &&
           a.max_distance == b.max_distance && a.vetoed == b.vetoed;
  }
};

enum class PlacerEvent : std::uint8_t {
  Move = 1,
  Confirm = 2,
  Cancel = 3,
  End = 4,
  // A click on a spot the rules refuse, for the client scripts' own sessions only (never sent:
  // Client/runtime/props/local_previews.hpp).
  Rejected = 5,
};

struct PlacerReport {
  PlacerEvent event = PlacerEvent::Move;
  std::uint32_t session = 0;
  bool valid = false;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  std::array<float, 3> normal{0.0f, 0.0f, 1.0f};
  float distance = 0.0f;
  bool on_surface = false;

  friend bool operator==(const PlacerReport& a, const PlacerReport& b) {
    return a.event == b.event && a.session == b.session && a.valid == b.valid &&
           a.position == b.position && a.rotation == b.rotation && a.normal == b.normal &&
           a.distance == b.distance && a.on_surface == b.on_surface;
  }
};

namespace preview_detail {

inline bool unit_range(const std::array<float, 4>& rgba) {
  for (const float channel : rgba) {
    if (!std::isfinite(channel) || channel < 0.0f || channel > 1.0f) {
      return false;
    }
  }
  return true;
}

inline bool valid_pieces(const std::vector<PreviewPiece>& pieces) {
  if (pieces.empty() || pieces.size() > kMaxPreviewPieces) {
    return false;
  }
  for (const PreviewPiece& piece : pieces) {
    if (!placed_detail::printable(piece.model, kMaxPlacedModelBytes) ||
        !placed_detail::finite(piece.offset) || !placed_detail::finite(piece.rotation)) {
      return false;
    }
  }
  return true;
}

inline void write_pieces(core::BitWriter& w, const std::vector<PreviewPiece>& pieces) {
  w.write_bits(std::uint8_t(pieces.size()), 8);
  for (const PreviewPiece& piece : pieces) {
    placed_detail::write_text(w, piece.model);
    placed_detail::write_floats(w, piece.offset);
    placed_detail::write_floats(w, piece.rotation);
  }
}

inline bool read_pieces(core::BitReader& r, std::vector<PreviewPiece>& pieces) {
  const std::size_t count = std::size_t(r.read_bits(8));
  if (r.overrun() || count == 0 || count > kMaxPreviewPieces) {
    return false;
  }
  pieces.resize(count);
  for (PreviewPiece& piece : pieces) {
    if (!placed_detail::read_text(r, kMaxPlacedModelBytes, piece.model)) {
      return false;
    }
    placed_detail::read_floats(r, piece.offset);
    placed_detail::read_floats(r, piece.rotation);
  }
  return !r.overrun() && valid_pieces(pieces);
}

inline void write_tint(core::BitWriter& w, const PreviewTint& tint) {
  w.write_bool(tint.present);
  if (tint.present) {
    placed_detail::write_floats(w, tint.rgba);
  }
}

inline bool read_tint(core::BitReader& r, PreviewTint& tint) {
  tint.present = r.read_bool();
  if (tint.present) {
    placed_detail::read_floats(r, tint.rgba);
  }
  return !r.overrun() && (!tint.present || unit_range(tint.rgba));
}

inline bool valid_scale(float scale) {
  return std::isfinite(scale) && scale > 0.0f && scale <= 100.0f;
}

inline bool valid_pose(const std::array<float, 3>& position, const std::array<float, 4>& rotation,
                       float scale) {
  return placed_detail::finite(position) && placed_detail::finite(rotation) && valid_scale(scale);
}

inline bool read_id(core::BitReader& r, std::uint32_t& id) {
  const std::uint64_t value = r.read_varuint();
  id = std::uint32_t(value);
  return !r.overrun() && value != 0 && value <= 0xffffffffull;
}

}  // namespace preview_detail

namespace preview_detail {

constexpr std::array<float, 3> kUniformAxes{1.0f, 1.0f, 1.0f};

inline bool valid_axes(const std::array<float, 3>& axes) {
  return valid_scale(axes[0]) && valid_scale(axes[1]) && valid_scale(axes[2]);
}

// The per-axis scale, written only when it is not uniform.
inline void write_axes(core::BitWriter& w, const std::array<float, 3>& axes) {
  if (axes != kUniformAxes) {
    w.write_bool(true);
    placed_detail::write_floats(w, axes);
  }
}

// Reads the per-axis scale when the message carries one (more than a byte's padding is left).
inline bool read_axes(core::BitReader& r, std::array<float, 3>& axes) {
  if (r.bits_remaining() < 1 + 3 * 32 || !r.read_bool()) {
    return true;
  }
  placed_detail::read_floats(r, axes);
  return valid_axes(axes);
}

}  // namespace preview_detail

// Empty when the order cannot travel.
inline std::vector<std::uint8_t> encode_ghost(const GhostOrder& order) {
  using namespace preview_detail;
  if (order.ghost == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kGhostTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.ghost);
  switch (order.kind) {
    case GhostKind::Create:
      if (!valid_pieces(order.pieces) || !valid_pose(order.position, order.rotation, order.scale) ||
          (order.tint.present && !unit_range(order.tint.rgba)) || !valid_axes(order.axes)) {
        return {};
      }
      write_pieces(w, order.pieces);
      placed_detail::write_floats(w, order.position);
      placed_detail::write_floats(w, order.rotation);
      placed_detail::write_float(w, order.scale);
      write_tint(w, order.tint);
      w.write_bool(order.hidden);
      write_axes(w, order.axes);
      break;
    case GhostKind::Pose:
      if (!valid_pose(order.position, order.rotation, order.scale) || !valid_axes(order.axes)) {
        return {};
      }
      placed_detail::write_floats(w, order.position);
      placed_detail::write_floats(w, order.rotation);
      placed_detail::write_float(w, order.scale);
      write_axes(w, order.axes);
      break;
    case GhostKind::Tint:
      if (order.tint.present && !unit_range(order.tint.rgba)) {
        return {};
      }
      write_tint(w, order.tint);
      break;
    case GhostKind::Hidden:
      w.write_bool(order.hidden);
      break;
    case GhostKind::Destroy:
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_ghost(const std::vector<std::uint8_t>& p, GhostOrder& order) {
  using namespace preview_detail;
  if (p.size() < 3 || p[0] != kGhostTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  GhostOrder decoded;
  decoded.kind = GhostKind(r.read_bits(8));
  if (!read_id(r, decoded.ghost)) {
    return false;
  }
  bool ok = true;
  switch (decoded.kind) {
    case GhostKind::Create:
      ok = read_pieces(r, decoded.pieces);
      placed_detail::read_floats(r, decoded.position);
      placed_detail::read_floats(r, decoded.rotation);
      decoded.scale = placed_detail::read_float(r);
      ok = ok && read_tint(r, decoded.tint);
      decoded.hidden = r.read_bool();
      ok = ok && valid_pose(decoded.position, decoded.rotation, decoded.scale);
      ok = ok && read_axes(r, decoded.axes);
      break;
    case GhostKind::Pose:
      placed_detail::read_floats(r, decoded.position);
      placed_detail::read_floats(r, decoded.rotation);
      decoded.scale = placed_detail::read_float(r);
      ok = valid_pose(decoded.position, decoded.rotation, decoded.scale);
      ok = ok && read_axes(r, decoded.axes);
      break;
    case GhostKind::Tint:
      ok = read_tint(r, decoded.tint);
      break;
    case GhostKind::Hidden:
      decoded.hidden = r.read_bool();
      break;
    case GhostKind::Destroy:
      break;
    default:
      return false;
  }
  if (!ok || r.overrun()) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

inline std::vector<std::uint8_t> encode_placer(const PlacerOrder& order) {
  using namespace preview_detail;
  if (order.session == 0) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPlacerTag, 8);
  w.write_bits(std::uint8_t(order.kind), 8);
  w.write_varuint(order.session);
  switch (order.kind) {
    case PlacerKind::Begin: {
      const std::array<float, 7> numbers{
          order.range,     order.fallback_distance, order.vertical_offset, order.yaw,
          order.max_slope, order.min_distance,      order.max_distance};
      if (!valid_pieces(order.pieces) || !valid_scale(order.scale) ||
          !unit_range(order.valid_tint) || !unit_range(order.invalid_tint) ||
          !placed_detail::finite(numbers) || order.range <= 0.0f ||
          order.range > kMaxPreviewRange) {
        return {};
      }
      write_pieces(w, order.pieces);
      placed_detail::write_float(w, order.scale);
      placed_detail::write_floats(w, order.valid_tint);
      placed_detail::write_floats(w, order.invalid_tint);
      placed_detail::write_float(w, order.range);
      placed_detail::write_float(w, order.fallback_distance);
      placed_detail::write_float(w, order.vertical_offset);
      placed_detail::write_float(w, order.yaw);
      w.write_bits(order.flags, 8);
      placed_detail::write_float(w, order.max_slope);
      placed_detail::write_float(w, order.min_distance);
      placed_detail::write_float(w, order.max_distance);
      break;
    }
    case PlacerKind::Veto:
      w.write_bool(order.vetoed);
      break;
    case PlacerKind::End:
      break;
    default:
      return {};
  }
  return w.bytes();
}

inline bool decode_placer(const std::vector<std::uint8_t>& p, PlacerOrder& order) {
  using namespace preview_detail;
  if (p.size() < 3 || p[0] != kPlacerTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  PlacerOrder decoded;
  decoded.kind = PlacerKind(r.read_bits(8));
  if (!read_id(r, decoded.session)) {
    return false;
  }
  bool ok = true;
  switch (decoded.kind) {
    case PlacerKind::Begin: {
      ok = read_pieces(r, decoded.pieces);
      decoded.scale = placed_detail::read_float(r);
      placed_detail::read_floats(r, decoded.valid_tint);
      placed_detail::read_floats(r, decoded.invalid_tint);
      decoded.range = placed_detail::read_float(r);
      decoded.fallback_distance = placed_detail::read_float(r);
      decoded.vertical_offset = placed_detail::read_float(r);
      decoded.yaw = placed_detail::read_float(r);
      decoded.flags = std::uint8_t(r.read_bits(8));
      decoded.max_slope = placed_detail::read_float(r);
      decoded.min_distance = placed_detail::read_float(r);
      decoded.max_distance = placed_detail::read_float(r);
      const std::array<float, 7> numbers{
          decoded.range,     decoded.fallback_distance, decoded.vertical_offset, decoded.yaw,
          decoded.max_slope, decoded.min_distance,      decoded.max_distance};
      ok = ok && valid_scale(decoded.scale) && unit_range(decoded.valid_tint) &&
           unit_range(decoded.invalid_tint) && placed_detail::finite(numbers) &&
           decoded.range > 0.0f && decoded.range <= kMaxPreviewRange;
      break;
    }
    case PlacerKind::Veto:
      decoded.vetoed = r.read_bool();
      break;
    case PlacerKind::End:
      break;
    default:
      return false;
  }
  if (!ok || r.overrun()) {
    return false;
  }
  order = std::move(decoded);
  return true;
}

inline std::vector<std::uint8_t> encode_placer_report(const PlacerReport& report) {
  const bool has_pose = report.event == PlacerEvent::Move || report.event == PlacerEvent::Confirm;
  if (report.session == 0 || report.event < PlacerEvent::Move || report.event > PlacerEvent::End) {
    return {};
  }
  if (has_pose &&
      (!placed_detail::finite(report.position) || !placed_detail::finite(report.rotation) ||
       !placed_detail::finite(report.normal) || !std::isfinite(report.distance))) {
    return {};
  }
  core::BitWriter w;
  w.write_bits(kPlacerReportTag, 8);
  w.write_bits(std::uint8_t(report.event), 8);
  w.write_varuint(report.session);
  w.write_bool(report.valid);
  if (has_pose) {
    placed_detail::write_floats(w, report.position);
    placed_detail::write_floats(w, report.rotation);
    placed_detail::write_floats(w, report.normal);
    placed_detail::write_float(w, report.distance);
    w.write_bool(report.on_surface);
  }
  return w.bytes();
}

inline bool decode_placer_report(const std::vector<std::uint8_t>& p, PlacerReport& report) {
  if (p.size() < 3 || p[0] != kPlacerReportTag) {
    return false;
  }
  core::BitReader r(p);
  r.read_bits(8);
  PlacerReport decoded;
  decoded.event = PlacerEvent(r.read_bits(8));
  if (decoded.event < PlacerEvent::Move || decoded.event > PlacerEvent::End ||
      !preview_detail::read_id(r, decoded.session)) {
    return false;
  }
  decoded.valid = r.read_bool();
  const bool has_pose = decoded.event == PlacerEvent::Move || decoded.event == PlacerEvent::Confirm;
  if (has_pose) {
    placed_detail::read_floats(r, decoded.position);
    placed_detail::read_floats(r, decoded.rotation);
    placed_detail::read_floats(r, decoded.normal);
    decoded.distance = placed_detail::read_float(r);
    decoded.on_surface = r.read_bool();
  }
  if (r.overrun() || !placed_detail::finite(decoded.position) ||
      !placed_detail::finite(decoded.rotation) || !placed_detail::finite(decoded.normal) ||
      !std::isfinite(decoded.distance)) {
    return false;
  }
  report = decoded;
  return true;
}

}  // namespace kcd2s::integration
