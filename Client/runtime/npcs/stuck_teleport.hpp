// The game's teleport of a stuck walker stays off in a server's world. With
// wh_ai_MovementTeleportWhenStuck on ("automatic teleport when the agent is stuck"; 2, waits until
// the actor and the target are not visible from the camera) the game client that walks a server
// NPC put the body at its goal as soon as its own camera did not see it, and every other player
// saw the person jump up to 10 m. Off, a stuck walk ends blocked and the server's order decides.
#pragma once

namespace kcds::runtime::npcs::stuck_teleport {

// Keeps the teleport off. Engine thread, every frame; it looks once a second.
void keep_off();

}  // namespace kcds::runtime::npcs::stuck_teleport
