// The props and effects the server placed.
#pragma once

#include <cstdint>

#include "kcd2s/integration/placed_objects.hpp"

namespace kcds::game_client {

// The game entity of a player's or an NPC's body by its network id, or null.
void* placed_body_entity(std::uint32_t network_id);
// The server's prop and effect orders, in the order it sent them, the reports of the rigid props
// this game simulates, and the bursts whose time is over.
void tick_placed_objects();
// Sends the server where a rigid prop this game simulates is.
bool prop_report(const kcd2s::integration::PropReport& report);

}  // namespace kcds::game_client
