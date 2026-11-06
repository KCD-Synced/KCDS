// The server's siege ladders in this game (kcd2s/integration/ladders_stones.hpp).
//
// A standing ladder is the game's own climbable ladder: an entity of the script class Ladder
// (Scripts/Entities/Others/Ladder.lua, Entities/Ladder.ent), spawned with the server's mesh as its
// Properties.fileModel and the server's height as Properties.height before its init, so its own
// OnSpawn loads and physicalises that mesh and its GetActions offers the game's climb
// (CanUseLadder, GrabOnLadder: the game's ladder action C_ActorActionLadder). It stands at the
// server's foot position, turned by the server's rotation and tilted `lean` degrees about its own
// x axis (its top towards -y, where the wall is).
//
// A ladder that is not standing is a BasicEntity prop of the same mesh (props::entity), turning
// about its foot: pushed, it stays at its lean for the shove, then falls away from the wall until
// it lies on the ground; raising, it rises from the ground back to its lean; down, it lies; broken,
// it lies as the mesh's broken variant (<mesh>_broken.cgf, the whole mesh when there is none).
// The climbable entity is gone while it is not standing, so the game ends a climb on it.
//
// Use: near the top of a standing, usable ladder the game's "use" hint offers the push; near a
// usable ladder lying on the ground, the raise. The server decides (siegeLadderUse) and sends the
// pose back; nothing changes here before it does. The game of the step's actor reports the end of
// the fall or the lift (siegeLadderSettled).
//
// Engine thread, except take_reports (any thread).
#pragma once

#include <vector>

#include "kcd2s/integration/ladders_stones.hpp"

namespace kcds::runtime::siege::ladders {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// Spawns, poses or removes a ladder, or takes the server's refusal of the player's ask.
void apply(const kcd2s::integration::SiegeLadderOrder& order, double now);

// Every frame: turns the falling and rising ladders and offers the push and the raise. `player` is
// the local player's position (null when there is none); `allowed` is false while a menu, the chat
// or another UI owns the input, or the player is on a ladder.
void frame(double now, const float* player, bool allowed);

// What the player asked of the ladders and the steps this game saw end, since the last call, to
// send to the server.
std::vector<kcd2s::integration::SiegeLadderReport> take_reports();

// The world is gone: no ladders, no hint.
void reset();

}  // namespace kcds::runtime::siege::ladders
