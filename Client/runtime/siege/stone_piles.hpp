// The server's stone piles in this game (kcd2s/integration/ladders_stones.hpp).
//
// The game's own stone throwing (the StonePile game object, C_StoneThrowingPile, and its
// minigame C_StoneThrowing behind StoneThrowing.CanUse and StartMinigame) cannot be given a pile
// of the server's: the pile works only with the stone entities and the StoneThrowingNode path the
// level links to it in the editor (sorts its entity links into stones, path nodes and
// targets; CanUse, refuses "The pile is invalid: no stones or no path" without them).
// So a pile here is drawn and thrown by the client:
//
//   pile   up to nine BasicEntity props of the game's hurling stones
//          (objects/manmade/task_specific_props/combat/castle_siege/stone_hurling/
//          hurling_stone.cgf and hurling_stone_b.cgf), laid as the game's own prefab
//          hurlingStone/hurlingStonePile_a lays them, turned by the pile's rotation; as many as
//          the pile has stones left (all nine for one that never runs out, none when it is empty).
//   throw  near a usable pile with stones the game's "use" hint offers a throw; the player's game
//          reports the take (stonePileTake) and heaves a hurling stone from the player's head
//          along where the camera looks, falling as a thrown body falls. The game's own ray test
//          (physics::first_hit) between the stone's positions of two frames stops it where it
//          meets the world; that point goes to the server (stoneImpact), which decides the damage.
//
// Only the thrower's game draws its stone: the server sends no thrown stones to the others.
//
// Engine thread, except take_reports (any thread).
#pragma once

#include <vector>

#include "kcd2s/integration/ladders_stones.hpp"

namespace kcds::runtime::siege::stones {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// Spawns, changes or removes a pile.
void apply(const kcd2s::integration::StonePileOrder& order);

// Every frame: flies the player's stones and offers the throw. `player` is the local player's
// position (null when there is none), `view` the view camera's matrix (null when there is none);
// `allowed` is false while a menu, the chat or another UI owns the input.
void frame(double now, const float* player, const float* view, bool allowed);

// The stones the player took and where they came down, since the last call, to send to the
// server.
std::vector<kcd2s::integration::StonePileReport> take_reports();

// The world is gone: no piles, no stones, no hint.
void reset();

}  // namespace kcds::runtime::siege::stones
