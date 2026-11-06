// NoClip and the server's camera shots: views of the engine's own view
// system, their natives found and installed at startup together.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void install_noclip();

}  // namespace kcds::game_client
