#include "kcd2s/replication/interest.hpp"

#include <cmath>

namespace kcd2s::replication {

CellKey InterestGrid::pack(std::int32_t cx, std::int32_t cz) {
  return (static_cast<CellKey>(static_cast<std::uint32_t>(cx)) << 32) |
         static_cast<CellKey>(static_cast<std::uint32_t>(cz));
}

std::int32_t InterestGrid::cell_coord(float v) const {
  return static_cast<std::int32_t>(std::floor(v / cfg_.cell_size));
}

CellKey InterestGrid::cell_of(const Vec3& p) const {
  return pack(cell_coord(p.x), cell_coord(depth(p)));
}

float InterestGrid::horizontal_distance_sq(const Vec3& a, const Vec3& b) const {
  const float dx = a.x - b.x;
  const float dd = depth(a) - depth(b);
  return dx * dx + dd * dd;
}

void InterestGrid::erase_from_cell(EntityId e, CellKey key) {
  auto it = cells_.find(key);
  if (it == cells_.end())
    return;
  auto& vec = it->second;
  for (std::size_t i = 0; i < vec.size(); ++i) {
    if (vec[i] == e) {
      vec[i] = vec.back();
      vec.pop_back();
      break;
    }
  }
  if (vec.empty())
    cells_.erase(it);
}

void InterestGrid::set_position(EntityId e, const Vec3& pos) {
  const CellKey new_key = cell_of(pos);
  auto cit = cell_.find(e);
  if (cit == cell_.end()) {
    cells_[new_key].push_back(e);
    cell_[e] = new_key;
  } else if (cit->second != new_key) {
    erase_from_cell(e, cit->second);  // incremental: only the two cells touched
    cells_[new_key].push_back(e);
    cit->second = new_key;
  }
  pos_[e] = pos;
}

void InterestGrid::remove(EntityId e) {
  auto cit = cell_.find(e);
  if (cit != cell_.end()) {
    erase_from_cell(e, cit->second);
    cell_.erase(cit);
  }
  pos_.erase(e);
}

std::vector<EntityId> InterestGrid::query(const Vec3& viewer, float range,
                                          const Vec3& velocity) const {
  std::vector<EntityId> out;
  const float range_sq = range * range;
  const bool has_vel = (velocity.x != 0.0f || depth(velocity) != 0.0f);
  Vec3 ahead{viewer.x + velocity.x, viewer.y, viewer.z};
  if (cfg_.up == UpAxis::Y)
    ahead.z += velocity.z;
  else
    ahead.y += velocity.y;

  // Inclusion test: within range of the viewer, or (with a velocity) of the lookahead point.
  auto consider = [&](EntityId e) {
    const Vec3& p = pos_.at(e);
    bool in = horizontal_distance_sq(p, viewer) <= range_sq;
    if (!in && has_vel)
      in = horizontal_distance_sq(p, ahead) <= range_sq;
    if (in) {
      out.push_back(e);
    }
  };

  // Cost of scanning the cell box around a center vs. iterating all occupied cells: pick whichever
  // is cheaper. A tight range scans a small box; a huge range (e.g. "see everything") would scan an
  // enormous mostly-empty box, so fall back to iterating the occupied cells instead.
  const double radius = std::ceil(range / cfg_.cell_size);
  const double side = 2.0 * radius + 1.0;
  double boxes = side * side;
  if (has_vel)
    boxes *= 2.0;

  if (boxes <= static_cast<double>(cells_.size())) {
    const int r = static_cast<int>(radius);
    const std::int32_t first_x = cell_coord(viewer.x);
    const std::int32_t first_z = cell_coord(depth(viewer));
    auto scan_box = [&](const Vec3& center, bool skip_first_box) {
      const std::int32_t ccx = cell_coord(center.x);
      const std::int32_t ccz = cell_coord(depth(center));
      for (std::int32_t dx = -r; dx <= r; ++dx)
        for (std::int32_t dz = -r; dz <= r; ++dz) {
          // Each entity belongs to exactly one cell. Only overlapping lookahead
          // boxes could visit it twice; skip those cells on the second pass.
          // Both distance tests already ran on the first pass, so this keeps the
          // previous first-encounter order without allocating an entity set.
          const auto from_first_x = std::int64_t(ccx) + dx - first_x;
          const auto from_first_z = std::int64_t(ccz) + dz - first_z;
          if (skip_first_box && from_first_x >= -std::int64_t(r) && from_first_x <= r &&
              from_first_z >= -std::int64_t(r) && from_first_z <= r) {
            continue;
          }
          auto it = cells_.find(pack(ccx + dx, ccz + dz));
          if (it == cells_.end())
            continue;
          for (EntityId e : it->second)
            consider(e);
        }
    };
    scan_box(viewer, false);
    if (has_vel)
      scan_box(ahead, true);
  } else {
    // Occupied cells and their members are unique, even when lookahead is active.
    for (const auto& [key, bucket] : cells_)
      for (EntityId e : bucket)
        consider(e);
  }
  return out;
}

float base_priority(Priority p, const ScoreParams& sp) {
  switch (p) {
    case Priority::Low:
      return sp.base_low;
    case Priority::Med:
      return sp.base_med;
    case Priority::High:
      return sp.base_high;
  }
  return sp.base_med;
}

float score_entity(Priority base, Tick now, Tick last_sent, float distance, bool flags_important,
                   const ScoreParams& sp) {
  const float recency = static_cast<float>(now - last_sent) * sp.recency_weight;
  const float dist_penalty = distance * sp.distance_weight;
  const float importance = flags_important ? sp.importance_boost : 0.0f;
  return base_priority(base, sp) + recency - dist_penalty + importance;
}

}  // namespace kcd2s::replication
