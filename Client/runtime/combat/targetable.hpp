// Players a client script keeps out of this machine's combat lock-on (Player.setLockable, issue).
// The combat eligibility hook (combat_input.cpp) no longer makes such a player's body an
// enemy for a lock-on request or a held selection, so the game's own picker passes over it and
// lets go of it; a swing in progress still lands, damage being the server's to refuse.
//
// The client runtime sends { op: 'playerTargetable', id, targetable } (Client/resources/
// entities.cjs); the choice lasts until changed or the session ends. Any thread.
#pragma once

#include <cstdint>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::combat::targetable {

// A resource runtime message; false when it is not this one.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Whether a script took the player of that network id out of the lock-on.
bool untargetable(std::uint32_t player);

// The session ended: every player is a target again.
void reset();

}  // namespace kcds::runtime::combat::targetable
