// Props: held props, the props of script animations, the
// props, effects and carts the server places, and the player's previews; their natives found
// and installed at startup.
#pragma once

#include "held_props.hpp"

namespace kcds::game_client {

// Called by initialize_adapter(): the held props and the animation props. Returns the props'
// natives, which the crafting stations and the placed objects share (attach_child, physics).
kcds::runtime::props::Natives install_props();
// Called by initialize_adapter() with the props' natives: the placed props, effects and carts,
// and the player's previews and placement.
void install_placed_objects(const kcds::runtime::props::Natives& natives);
// Called by initialize_adapter(): the props, then the crafting stations and the placed objects
// on the props' natives.
void install_props_and_crafting();

}  // namespace kcds::game_client
