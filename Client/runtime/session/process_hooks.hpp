// The process around a server session: the departure announced from the process exit calls, and
// no game pause in a multiplayer world.
#pragma once

#include <cstdint>

namespace kcds::game_client {

void install_departure_hooks();
// CCryAction::PauseGame (hooked: refused in a multiplayer world).
using PauseGameFn = void(__fastcall*)(void*, bool, std::uint16_t, bool);
extern PauseGameFn original_pause_game;
void __fastcall hooked_pause_game(void* framework, bool pause, std::uint16_t reason, bool force);

}  // namespace kcds::game_client
