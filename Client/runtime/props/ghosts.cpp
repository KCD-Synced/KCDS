#include "ghosts.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>

#include "../gizmos/gizmos.hpp"
#include "pose_math.hpp"
#include "prop_entity.hpp"

namespace kcds::runtime::props::ghosts {
namespace {

using kcd2s::integration::GhostKind;
using kcd2s::integration::GhostOrder;

// The ring's radius per unit of scale.
constexpr float kRingRadius = 0.6f;

struct Ghost {
  Preview preview;
  bool hidden = false;
  // The box last reported to the client scripts (local ghosts only).
  bool reported = false;
  bool bounded = false;
  float min[3]{};
  float max[3]{};
};

Logger log_line = nullptr;
// Engine thread only: server ghost number -> the preview.
std::unordered_map<std::uint32_t, Ghost> ghosts;

std::string piece_name(const Preview& preview, std::size_t piece) {
  return preview.name + "_" + std::to_string(piece);
}

// The game's own see-through filter material (Illum at 0.7 opacity, IPL_GameData.pak).
const char* const kSeeThroughMaterial = "Materials/filters/fade_short_70";

float opacity_of(const Preview& preview) {
  return preview.tint.present ? preview.tint.rgba[3] : kGhostOpacity;
}

// Where a piece stands: the preview's pose, then the piece's scaled offset and its own turn.
void piece_pose(const Preview& preview, const kcd2s::integration::PreviewPiece& piece,
                float position[3], float rotation[4]) {
  float scaled[3];
  for (int axis = 0; axis < 3; ++axis) {
    scaled[axis] = piece.offset[axis] * preview.scale * preview.axes[axis];
  }
  float turned[3];
  pose_math::rotate(preview.rotation, scaled, turned);
  for (int axis = 0; axis < 3; ++axis) {
    position[axis] = preview.position[axis] + turned[axis];
  }
  pose_math::multiply(preview.rotation, piece.rotation.data(), rotation);
  pose_math::normalize(rotation);
}

void set_pose(void* body, const Preview& preview, const kcd2s::integration::PreviewPiece& piece) {
  float position[3];
  float rotation[4];
  piece_pose(preview, piece, position, rotation);
  const float scale[3] = {preview.scale * preview.axes[0], preview.scale * preview.axes[1],
                          preview.scale * preview.axes[2]};
  entity::place_rotated(body, position, rotation);
  entity::set_scale(body, scale);
}

void copy_pose(const GhostOrder& order, Preview& preview) {
  std::copy(order.position.begin(), order.position.end(), preview.position);
  std::copy(order.rotation.begin(), order.rotation.end(), preview.rotation);
  pose_math::normalize(preview.rotation);
  preview.scale = order.scale;
  std::copy(order.axes.begin(), order.axes.end(), preview.axes);
}

void create(const GhostOrder& order) {
  const auto found = ghosts.find(order.ghost);
  if (found != ghosts.end()) {
    hide(found->second.preview);
  }
  Ghost& ghost = ghosts[order.ghost];
  ghost = Ghost{};
  ghost.preview.name = "kcds_ghost_" + std::to_string(order.ghost);
  ghost.preview.pieces = order.pieces;
  ghost.preview.tint = order.tint;
  ghost.preview.see_through = order.see_through;
  copy_pose(order, ghost.preview);
  ghost.hidden = order.hidden;
  const bool shown = !ghost.hidden && show(ghost.preview);
  if (log_line) {
    log_line("GHOST %u %s (%zu meshes): %s", order.ghost, order.pieces.front().model.c_str(),
             order.pieces.size(),
             ghost.hidden ? "hidden"
             : shown      ? "shown"
                          : "not shown");
  }
}

}  // namespace

bool show(Preview& preview) {
  hide(preview);
  bool any = false;
  for (std::size_t i = 0; i < preview.pieces.size(); ++i) {
    const auto& piece = preview.pieces[i];
    const std::string material = preview.see_through ? kSeeThroughMaterial : std::string();
    void* body =
        entity::spawn_mesh(piece_name(preview, i), preview.position, piece.model, material);
    if (!body) {
      continue;
    }
    set_pose(body, preview, piece);
    entity::set_opacity(body, opacity_of(preview));
    any = true;
  }
  preview.shown = any;
  return any;
}

void place(const Preview& preview) {
  if (!preview.shown) {
    return;
  }
  for (std::size_t i = 0; i < preview.pieces.size(); ++i) {
    if (void* body = entity::find(piece_name(preview, i))) {
      set_pose(body, preview, preview.pieces[i]);
    }
  }
}

void fade(const Preview& preview) {
  if (!preview.shown) {
    return;
  }
  for (std::size_t i = 0; i < preview.pieces.size(); ++i) {
    if (void* body = entity::find(piece_name(preview, i))) {
      entity::set_opacity(body, opacity_of(preview));
    }
  }
}

void hide(Preview& preview) {
  for (std::size_t i = 0; i < preview.pieces.size(); ++i) {
    entity::remove(piece_name(preview, i));
  }
  preview.shown = false;
}

void draw_ring(const Preview& preview) {
  if (!preview.shown || !preview.tint.present) {
    return;
  }
  const float color[4] = {preview.tint.rgba[0], preview.tint.rgba[1], preview.tint.rgba[2], 1.0f};
  gizmos::circle(preview.position, kRingRadius * preview.scale, color);
}

void install(Logger logger) {
  log_line = logger;
}

void apply(const GhostOrder& order) {
  if (order.kind == GhostKind::Create) {
    create(order);
    return;
  }
  const auto found = ghosts.find(order.ghost);
  if (found == ghosts.end()) {
    return;
  }
  Ghost& ghost = found->second;
  switch (order.kind) {
    case GhostKind::Pose:
      copy_pose(order, ghost.preview);
      place(ghost.preview);
      break;
    case GhostKind::Tint:
      ghost.preview.tint = order.tint;
      fade(ghost.preview);
      break;
    case GhostKind::Hidden:
      ghost.hidden = order.hidden;
      if (ghost.hidden) {
        hide(ghost.preview);
      } else if (!ghost.preview.shown) {
        show(ghost.preview);
      }
      break;
    case GhostKind::Destroy:
      hide(ghost.preview);
      if (log_line) {
        log_line("GHOST %u destroyed", order.ghost);
      }
      ghosts.erase(found);
      break;
    default:
      break;
  }
}

void frame() {
  for (const auto& [number, ghost] : ghosts) {
    (void)number;
    draw_ring(ghost.preview);
  }
}

std::vector<BoundsChange> take_local_bounds() {
  std::vector<BoundsChange> changes;
  for (auto& [number, ghost] : ghosts) {
    if ((number & 0x80000000u) == 0) {
      continue;
    }
    BoundsChange now;
    now.ghost = number;
    if (ghost.preview.shown) {
      for (std::size_t i = 0; i < ghost.preview.pieces.size(); ++i) {
        float low[3];
        float high[3];
        if (!entity::world_bounds(entity::find(piece_name(ghost.preview, i)), low, high)) {
          continue;
        }
        for (int k = 0; k < 3; ++k) {
          now.min[k] = now.present ? std::min(now.min[k], low[k]) : low[k];
          now.max[k] = now.present ? std::max(now.max[k], high[k]) : high[k];
        }
        now.present = true;
      }
    }
    // Reported when it changed by more than a millimetre, or came or went.
    bool changed = !ghost.reported || now.present != ghost.bounded;
    for (int k = 0; k < 3 && !changed && now.present; ++k) {
      changed = std::fabs(now.min[k] - ghost.min[k]) > 0.001f ||
                std::fabs(now.max[k] - ghost.max[k]) > 0.001f;
    }
    if (!changed) {
      continue;
    }
    ghost.reported = true;
    ghost.bounded = now.present;
    for (int k = 0; k < 3; ++k) {
      ghost.min[k] = now.min[k];
      ghost.max[k] = now.max[k];
    }
    changes.push_back(now);
  }
  return changes;
}

void reset() {
  ghosts.clear();
}

}  // namespace kcds::runtime::props::ghosts
