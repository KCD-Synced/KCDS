// The game's own single-player screens that step aside in a server world: the skip-time screen
// and the tutorials (Hud.showTutorial aside).
#pragma once

#include <cstdint>

namespace kcds::game_client {

using StartSkipTime = char(__fastcall*)(void*, unsigned, float, float, const void*, int);
extern StartSkipTime original_start_skip_time;
char __fastcall hooked_start_skip_time(void* self, unsigned mode, float a, float b,
                                       const void* state, int flags);

using TutorialsEnabled = bool(__fastcall*)();
extern TutorialsEnabled original_tutorials_enabled;
// Set while the resources' Hud calls run.
extern bool resource_tutorial_call;
bool __fastcall hooked_tutorials_enabled();
// The game's tutorial query (it reads wh_ui_TutorialsEnabled), found by its one literal.
void* resolve_tutorial_query();

// The native menu's button filter: no save or load buttons in a server world.
bool multiplayer_button_filter(std::uint8_t id);

}  // namespace kcds::game_client
