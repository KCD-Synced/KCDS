// Player connection slots: the value scripts read as Player.playerIndex..
//
// playerIndex is the connection slot a player holds: stable
// while the player stays connected, handed to a later joiner once they leave, and 65535 while no
// slot is assigned. That is deliberately different from Player.id, the network entity id, which
// a server session never reuses. A joining player gets the lowest free slot, starting at 0.
#ifndef KCD2S_INTEGRATION_PLAYER_SLOTS_HPP
#define KCD2S_INTEGRATION_PLAYER_SLOTS_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <queue>
#include <unordered_map>
#include <vector>

#include "kcd2s/core/ids.hpp"

namespace kcd2s::integration {

class PlayerSlots {
 public:
  static constexpr std::uint16_t kUnassigned = 0xFFFFu;

  // Assigns the lowest free slot to `player` and returns it. A player that already holds a slot
  // keeps it. Returns kUnassigned for kInvalidEntity or when all 65535 slots are taken.
  std::uint16_t assign(core::EntityId player);

  // Frees the player's slot so the next joiner can take it. No-op for a player without a slot.
  void release(core::EntityId player);

  // Slot held by `player`, or kUnassigned.
  std::uint16_t slot_of(core::EntityId player) const;

  std::size_t assigned_count() const {
    return slot_by_player_.size();
  }

 private:
  // Released slots, lowest first. Every slot below next_unused_ is either held or in here.
  std::priority_queue<std::uint16_t, std::vector<std::uint16_t>, std::greater<std::uint16_t>>
      released_;
  std::uint32_t next_unused_ = 0;
  std::unordered_map<core::EntityId, std::uint16_t> slot_by_player_;
};

}  // namespace kcd2s::integration

#endif  // KCD2S_INTEGRATION_PLAYER_SLOTS_HPP
