// Reusable membership of live entities during one immutable world phase.
#ifndef KCD2S_REPLICATION_ROW_MEMBERSHIP_HPP
#define KCD2S_REPLICATION_ROW_MEMBERSHIP_HPP

#include <algorithm>
#include <cstdint>
#include <type_traits>
#include <vector>

#include "kcd2s/replication/world.hpp"

namespace kcd2s::replication {

// A scratch object belongs to one synchronous build/chunk, never to a viewer or a public ID
// range. Dense rows are bounded by World::kMaxLiveEntities even after long-session ID churn.
// A narrow unsigned generation is useful for exercising wrap in tests.
template <typename Generation = std::uint32_t>
class BasicRowMembership {
 public:
  static_assert(std::is_unsigned_v<Generation> && !std::is_same_v<Generation, bool>);

  void begin(const World& world) {
    world_ = &world;
    if (entries_.size() < world.row_count()) {
      entries_.resize(world.row_count());
    }
    generation_ = Generation(generation_ + 1);
    if (generation_ == 0) {
      std::fill(entries_.begin(), entries_.end(), Entry{});
      generation_ = 1;
    }
  }

  bool insert(core::EntityId entity) {
    if (!world_) {
      return false;
    }
    const auto row = world_->row_index(entity);
    if (row >= entries_.size()) {
      return false;
    }
    auto& entry = entries_[row];
    if (entry.generation == generation_ && entry.entity == entity) {
      return false;
    }
    entry = {generation_, entity};
    return true;
  }

  bool contains(core::EntityId entity) const {
    if (!world_) {
      return false;
    }
    const auto row = world_->row_index(entity);
    if (row >= entries_.size()) {
      return false;
    }
    const auto& entry = entries_[row];
    // The identity guard also rejects a row recycled between phases. Dead IDs never resolve.
    return entry.generation == generation_ && entry.entity == entity;
  }

  bool belongs_to(const World& world) const {
    return world_ == &world;
  }

  std::size_t retained_rows() const {
    return entries_.size();
  }

 private:
  struct Entry {
    Generation generation = 0;
    core::EntityId entity = core::kInvalidEntity;
  };
  const World* world_ = nullptr;
  Generation generation_ = 0;
  std::vector<Entry> entries_;
};

using RowMembership = BasicRowMembership<>;

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_ROW_MEMBERSHIP_HPP
