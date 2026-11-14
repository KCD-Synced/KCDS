// A guard's answer to an attack whose attacker is gone (WHGame 1.5.5).
//
// The guard action's update asks, while it answers an attack (its attack id at + 124 is not -1),
// whether its opponent (the actor's combat core + 3320, the lock target) is in stealth: a small
// function that reads the opponent's + 0x1F8 without checking it. When the attacker's body went
// away during the block (a player who left, a replica taken out of the world) the lock target is
// null while the guard still answers the attack, and the read crashed the game (a player holding
// block, live). The hook answers "not in stealth" for a missing opponent, as the game does for
// an opponent that is not.
//
// Engine thread.
#pragma once

namespace kcds::runtime::combat::guard_opponent {

using Logger = void (*)(const char* format, ...);

// Hooks the opponent's stealth check; false when it was not found.
bool install(Logger log);

}  // namespace kcds::runtime::combat::guard_opponent
