// Crafting stations: smithing and alchemy seen by everyone, the
// stations' effects, sounds and emitters, and the server's permits; installed at startup
#pragma once

#include "../props/held_props.hpp"

namespace kcds::game_client {

// Called by initialize_adapter() with the props' natives (props/props_install.hpp), whose
// attach and physics calls the stations share.
void install_crafting(const kcds::runtime::props::Natives& natives);

}  // namespace kcds::game_client
