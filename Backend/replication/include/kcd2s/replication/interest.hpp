// Interest management: an incremental spatial hash (no periodic full
// rebuild) that answers Query(viewer) = the entities a viewer should see, plus a priority scorer
// that lets the snapshot builder spend a byte budget on the most important entities first.
#ifndef KCD2S_REPLICATION_INTEREST_HPP
#define KCD2S_REPLICATION_INTEREST_HPP

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "kcd2s/core/ids.hpp"
#include "kcd2s/core/math.hpp"
#include "kcd2s/replication/schema.hpp"

namespace kcd2s::replication {

using core::EntityId;
using core::Tick;
using core::Vec3;

// Packed (cell column, cell row) key of the horizontal plane.
using CellKey = std::uint64_t;

// Which axis points up. The grid hashes and measures the two remaining (horizontal) axes.
// KCD2 (CryEngine) world coordinates are Z-up, so the game host uses UpAxis::Z. The default stays
// Y-up for the engine-agnostic tests and tools that were written against the (x, z) plane.
enum class UpAxis : std::uint8_t { Y, Z };

struct InterestConfig {
  float cell_size = 32.0f;  // world units per cell edge
  UpAxis up = UpAxis::Y;
};

class InterestGrid {
 public:
  explicit InterestGrid(InterestConfig cfg = {}) : cfg_(cfg) {
  }

  CellKey cell_of(const Vec3& p) const;

  // Squared distance between two points in the horizontal plane (the up axis is ignored).
  float horizontal_distance_sq(const Vec3& a, const Vec3& b) const;
  const InterestConfig& config() const {
    return cfg_;
  }

  // Add or move an entity to the cell for `pos` (incremental: only touches the two affected cells
  // when the cell changes; a no-op bucket move when it doesn't).
  void set_position(EntityId e, const Vec3& pos);
  void remove(EntityId e);

  bool contains(EntityId e) const {
    return pos_.count(e) != 0;
  }
  std::size_t entity_count() const {
    return pos_.size();
  }
  std::size_t cell_count() const {
    return cells_.size();
  }

  // Entities whose horizontal distance to `viewer` is <= range. `velocity` adds a one-cell
  // lookahead along the movement direction. Results are
  // de-duplicated; order is unspecified.
  std::vector<EntityId> query(const Vec3& viewer, float range, const Vec3& velocity = {}) const;

 private:
  static CellKey pack(std::int32_t cx, std::int32_t cz);
  std::int32_t cell_coord(float v) const;
  // The second horizontal coordinate: z for a Y-up world, y for a Z-up world.
  float depth(const Vec3& p) const {
    return cfg_.up == UpAxis::Y ? p.z : p.y;
  }

  InterestConfig cfg_;
  std::unordered_map<CellKey, std::vector<EntityId>> cells_;
  std::unordered_map<EntityId, Vec3> pos_;
  std::unordered_map<EntityId, CellKey> cell_;

  void erase_from_cell(EntityId e, CellKey key);
};

// Priority scoring (§6): higher score = sent first under a byte budget.
//   score = basePrio + recency*(now-lastSent) - distance*dist + importance(flags)
struct ScoreParams {
  float base_low = 0.0f;
  float base_med = 10.0f;
  float base_high = 20.0f;
  float recency_weight = 0.5f;      // starved entities rise over time
  float distance_weight = 0.02f;    // farther entities fall
  float importance_boost = 100.0f;  // viewers/avatars/quest objects jump the queue
};

float base_priority(Priority p, const ScoreParams& sp = {});

// `flags_important` true => add the importance boost (e.g. a viewer/avatar/alwaysVisible entity).
float score_entity(Priority base, Tick now, Tick last_sent, float distance, bool flags_important,
                   const ScoreParams& sp = {});

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_INTEREST_HPP
