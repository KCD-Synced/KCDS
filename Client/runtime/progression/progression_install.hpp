// Progression, buffs and perks: the XP orders, the replicas' souls and buffs and the
// perk gate, their natives found and hooks installed at startup.
#pragma once

#include <windows.h>

namespace kcds::game_client {

// Called by initialize_adapter(). Returns 0, or 1 when the adapter must not start (its exit
// code).
DWORD install_progression();

}  // namespace kcds::game_client
