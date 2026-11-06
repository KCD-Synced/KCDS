// Point lights each frame: the server's light orders, then the client scripts'.
#pragma once

namespace kcds::game_client {

// Resolves the lights' natives (lights.hpp).
void install_lights();

void tick_lights();

}  // namespace kcds::game_client
