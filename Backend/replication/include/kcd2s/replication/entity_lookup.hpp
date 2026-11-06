// Entity id -> storage slot lookup for the server world.
//
// Public entity ids are sequential per server session and never reused, so they cannot index the
// dense slot storage directly. This table maps a live id to its slot with open addressing and
// linear probing over one contiguous array: a lookup never allocates and usually touches a single
// cache line. Only insert can allocate, when the table grows; erase uses backward-shift deletion,
// so no tombstones accumulate however many entities a long session spawns and despawns.
#ifndef KCD2S_REPLICATION_ENTITY_LOOKUP_HPP
#define KCD2S_REPLICATION_ENTITY_LOOKUP_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::replication {

class EntityLookup {
 public:
  static constexpr std::uint32_t kNotFound = 0xFFFFFFFFu;

  // Slot stored for `id`, or kNotFound when the id is not present (including kInvalidEntity).
  std::uint32_t find(core::EntityId id) const;

  // Adds id -> slot. `id` must be a valid id that is not already present.
  void insert(core::EntityId id, std::uint32_t slot);

  // Removes `id`. Returns false when it was not present.
  bool erase(core::EntityId id);

  std::size_t size() const {
    return size_;
  }

 private:
  struct Entry {
    core::EntityId id = core::kInvalidEntity;  // kInvalidEntity marks an empty entry
    std::uint32_t slot = 0;
  };

  std::size_t home_of(core::EntityId id) const;
  std::size_t position_of(core::EntityId id) const;  // entries_.size() when absent
  void grow();

  std::vector<Entry> entries_;  // capacity is zero or a power of two, kept at most half full
  std::size_t size_ = 0;
  unsigned capacity_bits_ = 0;
};

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_ENTITY_LOOKUP_HPP
