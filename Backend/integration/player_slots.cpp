#include "kcd2s/integration/player_slots.hpp"

namespace kcd2s::integration {

std::uint16_t PlayerSlots::assign(core::EntityId player) {
  if (player == core::kInvalidEntity) {
    return kUnassigned;
  }
  const auto existing = slot_by_player_.find(player);
  if (existing != slot_by_player_.end()) {
    return existing->second;
  }

  std::uint16_t index;
  if (!released_.empty()) {
    index = released_.top();
    released_.pop();
  } else if (next_unused_ < kUnassigned) {
    index = static_cast<std::uint16_t>(next_unused_);
    ++next_unused_;
  } else {
    return kUnassigned;  // every slot below the "unassigned" marker is taken
  }

  slot_by_player_.emplace(player, index);
  return index;
}

void PlayerSlots::release(core::EntityId player) {
  const auto held = slot_by_player_.find(player);
  if (held == slot_by_player_.end()) {
    return;
  }
  released_.push(held->second);
  slot_by_player_.erase(held);
}

std::uint16_t PlayerSlots::slot_of(core::EntityId player) const {
  const auto held = slot_by_player_.find(player);
  if (held == slot_by_player_.end()) {
    return kUnassigned;
  }
  return held->second;
}

}  // namespace kcd2s::integration
