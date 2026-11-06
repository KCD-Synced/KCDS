// The local player's looks the resources asked for (Player.setAppearance, setVisible and
// setDisguise) each frame.
#pragma once

#include <string>

namespace kcds::game_client {

void tick_appearance_requests(void* local_actor);
bool tick_body_visible(void* local_entity);
std::string tick_disguise(void* local_entity);

// Player.wearItem's pieces on the local player.
void tick_worn_only(void* local_actor, void* local_entity);

}  // namespace kcds::game_client
