// A map from entity ids to values, kept in one flat array (open addressing, linear probing),
// for per-client tables the server reads and writes for every entity of every snapshot, where
// std::unordered_map's node per entry and pointer chasing cost more than the work itself.
//
// Only lookups, writes and removals by id: it is never iterated, so the order of its entries can
// never reach a snapshot. Any value is allowed (no value is reserved to mean "absent"), and
// kInvalidEntity is a key like any other, kept beside the table.
#ifndef KCD2S_REPLICATION_ENTITY_MAP_HPP
#define KCD2S_REPLICATION_ENTITY_MAP_HPP

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::replication {

template <class Value>
class EntityMap {
 public:
  // The value stored for `id`, or nullptr when there is none.
  const Value* find(core::EntityId id) const {
    if (id == core::kInvalidEntity) {
      return has_invalid_ ? &invalid_value_ : nullptr;
    }
    const std::size_t position = position_of(id);
    return position == entries_.size() ? nullptr : &entries_[position].value;
  }

  Value* find(core::EntityId id) {
    return const_cast<Value*>(static_cast<const EntityMap&>(*this).find(id));
  }

  bool contains(core::EntityId id) const {
    return find(id) != nullptr;
  }

  // The value stored for `id`, made (value-initialised) when there is none.
  Value& operator[](core::EntityId id) {
    if (id == core::kInvalidEntity) {
      if (!has_invalid_) {
        has_invalid_ = true;
        invalid_value_ = Value{};
      }
      return invalid_value_;
    }
    std::size_t position = position_of(id);
    if (position != entries_.size()) {
      return entries_[position].value;
    }
    if ((size_ + 1) * 2 > entries_.size()) {
      grow();
    }
    const std::size_t mask = entries_.size() - 1;
    position = home_of(id);
    while (entries_[position].id != core::kInvalidEntity) {
      position = (position + 1) & mask;
    }
    entries_[position].id = id;
    entries_[position].value = Value{};
    ++size_;
    return entries_[position].value;
  }

  // Removes `id`; false when it was not there.
  bool erase(core::EntityId id) {
    if (id == core::kInvalidEntity) {
      const bool had = has_invalid_;
      has_invalid_ = false;
      invalid_value_ = Value{};
      return had;
    }
    std::size_t hole = position_of(id);
    if (hole == entries_.size()) {
      return false;
    }
    // Backward-shift deletion: later entries of the probe chain move into the hole, so every
    // remaining id is still reached from its home position without tombstones.
    const std::size_t mask = entries_.size() - 1;
    std::size_t next = hole;
    while (true) {
      next = (next + 1) & mask;
      if (entries_[next].id == core::kInvalidEntity) {
        break;
      }
      const std::size_t home = home_of(entries_[next].id);
      bool home_between_hole_and_next;
      if (hole <= next) {
        home_between_hole_and_next = hole < home && home <= next;
      } else {
        home_between_hole_and_next = hole < home || home <= next;
      }
      if (home_between_hole_and_next) {
        continue;
      }
      entries_[hole] = std::move(entries_[next]);
      hole = next;
    }
    entries_[hole] = Entry{};
    --size_;
    return true;
  }

  std::size_t size() const {
    return size_ + (has_invalid_ ? 1 : 0);
  }

  bool empty() const {
    return size() == 0;
  }

  // Removes everything and keeps the table's room, for a map refilled every tick.
  void clear() {
    if (size_ != 0) {
      for (auto& entry : entries_) {
        entry = Entry{};
      }
    }
    size_ = 0;
    has_invalid_ = false;
    invalid_value_ = Value{};
  }

  // Keeps the entries for which keep(id, value) is true and removes the rest. Which entries
  // remain does not depend on the order they are looked at in.
  template <class Keep>
  void retain_if(Keep keep) {
    std::vector<core::EntityId> removed;
    for (const auto& entry : entries_) {
      if (entry.id != core::kInvalidEntity && !keep(entry.id, entry.value)) {
        removed.push_back(entry.id);
      }
    }
    for (core::EntityId id : removed) {
      erase(id);
    }
    if (has_invalid_ && !keep(core::kInvalidEntity, invalid_value_)) {
      erase(core::kInvalidEntity);
    }
  }

  // The same ids with the same values, however the tables are laid out.
  friend bool operator==(const EntityMap& a, const EntityMap& b) {
    if (a.size() != b.size()) {
      return false;
    }
    if (a.has_invalid_ && !(b.has_invalid_ && a.invalid_value_ == b.invalid_value_)) {
      return false;
    }
    for (const auto& entry : a.entries_) {
      if (entry.id == core::kInvalidEntity) {
        continue;
      }
      const Value* other = b.find(entry.id);
      if (!other || !(*other == entry.value)) {
        return false;
      }
    }
    return true;
  }

  friend bool operator!=(const EntityMap& a, const EntityMap& b) {
    return !(a == b);
  }

 private:
  struct Entry {
    core::EntityId id = core::kInvalidEntity;  // kInvalidEntity marks an empty entry
    Value value{};
  };

  static constexpr unsigned kMinimumCapacityBits = 4;  // 16 entries
  // 2^64 divided by the golden ratio: spreads sequential ids evenly over the table.
  static constexpr std::uint64_t kFibonacciMultiplier = 0x9E3779B97F4A7C15ull;

  std::size_t home_of(core::EntityId id) const {
    const std::uint64_t mixed = static_cast<std::uint64_t>(id) * kFibonacciMultiplier;
    return static_cast<std::size_t>(mixed >> (64u - capacity_bits_));
  }

  // The entry of `id`, or entries_.size() when it is not there.
  std::size_t position_of(core::EntityId id) const {
    if (entries_.empty()) {
      return entries_.size();
    }
    const std::size_t mask = entries_.size() - 1;
    std::size_t position = home_of(id);
    // The table is never more than half full, so the probe always reaches an empty entry.
    while (entries_[position].id != core::kInvalidEntity) {
      if (entries_[position].id == id) {
        return position;
      }
      position = (position + 1) & mask;
    }
    return entries_.size();
  }

  void grow() {
    std::vector<Entry> previous = std::move(entries_);
    capacity_bits_ = previous.empty() ? kMinimumCapacityBits : capacity_bits_ + 1;
    entries_.assign(std::size_t{1} << capacity_bits_, Entry{});
    size_ = 0;
    const std::size_t mask = entries_.size() - 1;
    for (auto& entry : previous) {
      if (entry.id == core::kInvalidEntity) {
        continue;
      }
      std::size_t position = home_of(entry.id);
      while (entries_[position].id != core::kInvalidEntity) {
        position = (position + 1) & mask;
      }
      entries_[position] = std::move(entry);
      ++size_;
    }
  }

  std::vector<Entry> entries_;  // capacity is zero or a power of two, kept at most half full
  std::size_t size_ = 0;
  unsigned capacity_bits_ = 0;
  bool has_invalid_ = false;
  Value invalid_value_{};
};

}  // namespace kcd2s::replication

#endif  // KCD2S_REPLICATION_ENTITY_MAP_HPP
