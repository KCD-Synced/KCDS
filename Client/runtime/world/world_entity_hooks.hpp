// The server's entities in this game's world: the NPCs' bodies, the client resources' own NPCs,
// interactions on things, siege engines, ladders and stone piles; their natives found in the game
// (split out of game_client.cpp's start).
#pragma once

namespace kcds::game_client {

// Once, as the client starts.
void install_world_entity_hooks();

}  // namespace kcds::game_client
