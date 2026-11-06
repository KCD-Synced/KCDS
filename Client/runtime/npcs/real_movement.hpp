// The simulator's NPC bodies walk with the game's real movement (issue 016).
//
// The game's MovementSystem (gAIEnv + 0x48) keeps a MovementActor for each body
// that moves (328 bytes each, a vector at + 80). Its byte at + 313 sends every walk of that body
// to C_FakeMovementManager (the gate reads it through MovementActor's slot 14),
// which moves the body in jumps a second apart and without its walk. The game sets
// it with MovementSystem's slot 25 (actor id, on/off) when its AI LOD takes a body
// down. The level's people and the server's NPCs had it set near the player, so on the game
// that simulates them they walked in jumps, and every other game showed them teleporting
// (the FAKEMOVE probe: every request came through this gate).
//
// keep_real() turns it off for one body through the same slot 25, only when the body has it
// on: off, the slot also resets the actor's fake movement.
//
// Engine thread.
#pragma once

#include <cstdint>

namespace kcds::runtime::npcs::real_movement {

using Logger = void (*)(const char* format, ...);

// Finds the MovementSystem global; false when the game's code is not found.
bool install(Logger logger);

// The body of entity `engine_id` walks with the game's real movement. True when it had been
// sent to fake movement and was taken back now.
bool keep_real(std::uint32_t engine_id);

}  // namespace kcds::runtime::npcs::real_movement
