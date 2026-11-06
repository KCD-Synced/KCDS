// Weapons in the hands, shots with bows and crossbows and items carried in the
// arms: their natives found and installed at startup together, since the shots and the
// carries use the weapons' item natives.
#pragma once

namespace kcds::game_client {

// Called by initialize_adapter().
void install_weapons();

}  // namespace kcds::game_client
