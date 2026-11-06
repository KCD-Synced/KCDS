// NativeUI: the game's own UI elements, as its UIAction script binding drives them;
// their natives found and installed at startup.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void install_native_ui();

}  // namespace kcds::game_client
