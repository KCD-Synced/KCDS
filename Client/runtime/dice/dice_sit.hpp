// Players sit down at a dice table with the game's own animation, WHGame 1.5.5.
//
// In the game alone a player sits at a dice table through the table's behaviour
// (so_diceTable_new.xml): after an NPC's dialogue it adds the interrupt "interrupt_playerSit"
// (the tree dice_interrupt_playerSit: the table's seat1_SO stance, sitting, then a wait for the
// minigame) to "__player". A match between players skips the dialogue, so the game seated them
// straight into the minigame. Now each player's client adds that interrupt to its local player
// the way the behaviour's AddInterrupt node does: the behaviour's id by its name
// (the game's string made and freed), the table's behaviour
// holder as host (by its WUID), the request (id, the host's id, priority 201,
// urgency Default; Instant seats the player without the animation, live), moved into
// the player's scheduler (C_NPC slot 0x138, the enqueue) and the request destroyed
// The player walks to the seat and plays the game's sit-down (clips 14807 or 14819 by
// the side, live), which every other client shows through the animation sync; the match's
// minigame starts once the player holds the seat and the sit-down is done (at most ten seconds).
// After the match the player stays seated and stands up with the game's own W.
//
// The table's second chair (seat2_SO) is the tree dice_interrupt_playDiceMinigame's
// ("interrupt_playDiceMinigame", the same sitting stance): the match's second player sits there,
// so each player has their own chair. The server may sit a player down before a match
// (DiceState Sit): they wait seated until a second player sits on the other chair.
//
// The tree's Synchronize waits for three participants (the table, an NPC, the player) and fails
// after its 20 s; live, the seated players stayed in the match and seated past it, and a player
// waiting alone stayed seated past it too (30 s).
//
// Engine thread only.
#pragma once

#include <cstdint>

namespace kcds::runtime::dice::sit {

using Logger = void (*)(const char* format, ...);
using EntityWuid = bool (*)(void* entity, std::uint64_t& wuid);

struct Natives {
  void* make_string = nullptr;
  void* free_string = nullptr;
  void* behavior_id = nullptr;
  void* host_of = nullptr;
  void* build_request = nullptr;
  void* move_request = nullptr;
  void* enqueue = nullptr;
  void* destroy_request = nullptr;
  void* const* npc_manager = nullptr;
  EntityWuid wuid_of = nullptr;
  Logger log = nullptr;
};

bool install(const Natives& natives);

// Adds the table's sit-down interrupt for chair `seat` (0 or 1) to the local player; false when
// it could not go (a match then starts at once, as before).
bool sit_down(void* table_behavior, std::uint8_t seat);

// The same for the server's NPC opponent: the table's interrupt for chair `seat` on
// the NPC's own agent, whose brain then sits it down. False when it could not go.
bool seat_npc(void* table_behavior, void* npc_agent, std::uint8_t seat);

// Takes the local player out of their chair at the table (the table's instantlyLeaveDiceTable,
// the interrupt the game gives both gamblers when a game ends). False when it could
// not go.
bool leave_table(void* table_behavior);

// The same for the NPC opponent the local game sat down (seat_npc): a script's NPC stayed in its
// chair after the match. False when it could not go.
bool npc_leave_table(void* table_behavior, void* npc_agent);

}  // namespace kcds::runtime::dice::sit
