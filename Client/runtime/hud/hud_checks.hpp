// The HUD's engine checks at startup: the console's script execution, the dialogue
// state and the view camera, each verified or resolved and logged.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter() before the resource Hud is installed.
void verify_engine_scripts();
// Called by initialize_adapter() after the resource Hud is installed.
void resolve_dialogue_and_view_camera();

}  // namespace kcds::game_client
