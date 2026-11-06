#include "kcd2s/integration/player_slots.hpp"

#include <cstddef>
#include <cstdint>

#include "kcd2s/testkit.hpp"

using kcd2s::core::EntityId;
using kcd2s::integration::PlayerSlots;

KCD2S_TEST(player_slots, joiners_take_the_lowest_free_slot) {
  PlayerSlots slots;
  KCD2S_CHECK_EQ(slots.assign(EntityId{1}), std::uint16_t{0});
  KCD2S_CHECK_EQ(slots.assign(EntityId{2}), std::uint16_t{1});
  KCD2S_CHECK_EQ(slots.slot_of(EntityId{1}), std::uint16_t{0});
  KCD2S_CHECK_EQ(slots.slot_of(EntityId{2}), std::uint16_t{1});
  // Assigning again keeps the slot stable for the session.
  KCD2S_CHECK_EQ(slots.assign(EntityId{1}), std::uint16_t{0});
  KCD2S_CHECK_EQ(slots.assigned_count(), std::size_t{2});
}

KCD2S_TEST(player_slots, reconnect_reuses_the_slot_but_not_the_id) {
  // Player 1 leaves; on reconnect they are a new entity (id 3) and take the freed slot 0, which
  // the contract allows. The old id no longer holds any slot.
  PlayerSlots slots;
  slots.assign(EntityId{1});
  slots.assign(EntityId{2});
  slots.release(EntityId{1});
  KCD2S_CHECK_EQ(slots.slot_of(EntityId{1}), PlayerSlots::kUnassigned);
  KCD2S_CHECK_EQ(slots.assign(EntityId{3}), std::uint16_t{0});
  KCD2S_CHECK_EQ(slots.slot_of(EntityId{2}), std::uint16_t{1});
  KCD2S_CHECK_EQ(slots.assign(EntityId{4}), std::uint16_t{2});
}

KCD2S_TEST(player_slots, invalid_and_unknown_players_are_unassigned) {
  PlayerSlots slots;
  KCD2S_CHECK_EQ(slots.assign(kcd2s::core::kInvalidEntity), PlayerSlots::kUnassigned);
  KCD2S_CHECK_EQ(slots.slot_of(EntityId{7}), PlayerSlots::kUnassigned);
  slots.release(EntityId{7});  // no-op
  KCD2S_CHECK_EQ(slots.assigned_count(), std::size_t{0});
}

KCD2S_TEST(player_slots, full_table_refuses_without_reaching_the_unassigned_marker) {
  PlayerSlots slots;
  bool sequential = true;
  for (std::uint32_t i = 0; i < PlayerSlots::kUnassigned; ++i) {
    sequential = sequential && slots.assign(EntityId{i + 1}) == static_cast<std::uint16_t>(i);
  }
  KCD2S_CHECK(sequential);
  KCD2S_CHECK_EQ(slots.assign(EntityId{0x20000u}), PlayerSlots::kUnassigned);
  slots.release(EntityId{100});
  KCD2S_CHECK_EQ(slots.assign(EntityId{0x20000u}), std::uint16_t{99});
}
