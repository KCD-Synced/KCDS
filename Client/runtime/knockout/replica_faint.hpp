// A replica never faints by its own game.
//
// Its owner's exhaust is kept on it (stats::keep_replica_vitals), so a tired owner's replica also
// holds the game's "exhausted" buff in every other game. That buff's faint check
// (C_ExhaustedBuffInstance, called by the buff's update) faints a soul
// that carries the player flag (soul + 0xCF0 bit 0x10) at its own random moment, and otherwise
// plays the player's "player_exhausted" effect. A replica lies down only when its owner faints
// (knockout::keep_replica_down), so the check does not run for a replica's soul at all; the local
// player's own and every other soul's run as the game made them.
#pragma once

namespace kcds::runtime::knockout::replica_faint {

using Logger = void (*)(const char* format, ...);
// Whether a soul is a replica's (thread-safe: the buff update's thread is the game's).
using IsReplicaSoul = bool (*)(void* soul);

// Hooks the faint check at `exhausted_check` (resolved and validated by the caller). False when
// it is missing or the hook fails; the game's own check then runs for every soul.
bool install(void* exhausted_check, IsReplicaSoul is_replica_soul, Logger logger);

}  // namespace kcds::runtime::knockout::replica_faint
