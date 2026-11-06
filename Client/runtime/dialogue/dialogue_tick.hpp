// The game's own conversation state and the server's conversations (Dialogue) each
// frame. read_game_dialogue and tick_dialogue are declared in core/client_internal.hpp too while
// the HUD calls still reach them from there.
#pragma once

namespace kcds::game_client {

bool read_game_dialogue();
void tick_dialogue(bool in_world);

}  // namespace kcds::game_client
