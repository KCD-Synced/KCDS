#include "kcd2s/replication/entity_lookup.hpp"

#include <utility>

namespace kcd2s::replication {

namespace {
constexpr unsigned kMinimumCapacityBits = 4;  // 16 entries
// 2^64 divided by the golden ratio. Multiplying by it spreads sequential ids evenly over the table.
constexpr std::uint64_t kFibonacciMultiplier = 0x9E3779B97F4A7C15ull;
}  // namespace

std::size_t EntityLookup::home_of(core::EntityId id) const {
  const std::uint64_t mixed = static_cast<std::uint64_t>(id) * kFibonacciMultiplier;
  return static_cast<std::size_t>(mixed >> (64u - capacity_bits_));
}

std::size_t EntityLookup::position_of(core::EntityId id) const {
  if (id == core::kInvalidEntity || entries_.empty()) {
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

std::uint32_t EntityLookup::find(core::EntityId id) const {
  const std::size_t position = position_of(id);
  if (position == entries_.size()) {
    return kNotFound;
  }
  return entries_[position].slot;
}

void EntityLookup::insert(core::EntityId id, std::uint32_t slot) {
  if ((size_ + 1) * 2 > entries_.size()) {
    grow();
  }
  const std::size_t mask = entries_.size() - 1;
  std::size_t position = home_of(id);
  while (entries_[position].id != core::kInvalidEntity) {
    position = (position + 1) & mask;
  }
  entries_[position].id = id;
  entries_[position].slot = slot;
  ++size_;
}

bool EntityLookup::erase(core::EntityId id) {
  std::size_t hole = position_of(id);
  if (hole == entries_.size()) {
    return false;
  }
  // Backward-shift deletion: pull later entries of the probe chain into the hole so every
  // remaining id is still reachable from its home position without tombstones.
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
      continue;  // the entry is already as close to its home as it can be
    }
    entries_[hole] = entries_[next];
    hole = next;
  }
  entries_[hole] = Entry{};
  --size_;
  return true;
}

void EntityLookup::grow() {
  std::vector<Entry> previous = std::move(entries_);
  capacity_bits_ = previous.empty() ? kMinimumCapacityBits : capacity_bits_ + 1;
  entries_.assign(std::size_t{1} << capacity_bits_, Entry{});
  size_ = 0;
  for (const Entry& entry : previous) {
    if (entry.id != core::kInvalidEntity) {
      insert(entry.id, entry.slot);
    }
  }
}

}  // namespace kcd2s::replication
