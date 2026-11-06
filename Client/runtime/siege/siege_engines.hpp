// The server's siege engines in this game (kcd2s/integration/siege.hpp).
//
// The game has no entity class of its own for a trebuchet or a cannon (no script entity, no
// native class: its sieges are cutscene props), so an engine is a BasicEntity prop named after its
// server id, spawned with props::entity:
//
//   trebuchet  the character Objects/characters/assets/trebuchet/trebuchet.cdf; its cycle plays
//              the clips of the game's own Mannequin fragments (kcd_trebuchet.adb):
//              quest_trebuchet_pull_back while drawing, quest_trebuchet_load while loading (its
//              last key held while ready) and quest_trebuchet_fire while firing, at the engine's
//              speed. Wrecked, its clip stops.
//   cannon     the static mesh Objects/manmade/task_specific_props/combat/cannon.cgf, and
//              cannon_broken.cgf while wrecked.
//
// A shot is drawn by every client: a stone (trebuchet_stone.cgf, or cannon_ammo.cgf for a
// cannon) flown along the arc the server gives (straight over the ground from `from` to
// `target` in `flight` seconds, rising as thrown up at the speed that tops out `apex` metres
// above `from`). The game's own ray test (physics::first_hit) between the stone's positions of
// two frames stops it where it meets the world; the shot's reporter (the operator's game when it
// is near the target: the server picks it) sends that point, or the target when nothing stopped
// it.
//
// Use: near a usable, empty, unwrecked engine the player is offered the game's "use" hint to
// work it; working it, the hint loads it when idle and fires it when ready, along where the camera
// looks. Walking away leaves it. The server decides every step (siegeEngineUse) and sends the
// state back; nothing changes here before it does.
//
// Engine thread, except take_uses and take_impacts (any thread).
#pragma once

#include <vector>

#include "kcd2s/integration/siege.hpp"

namespace kcds::runtime::siege {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// Spawns, changes or removes an engine.
void apply(const kcd2s::integration::SiegeEngineOrder& order);

// A shot was let go: its stone flies from now on.
void fire(const kcd2s::integration::SiegeShot& shot, double now);

// Every frame: flies the stones, and offers the use of the engines. `player` is the local
// player's position (null when there is none), `view` the view camera's matrix (null when there
// is none); `allowed` is false while a menu, the chat or another UI owns the input.
void frame(double now, const float* player, const float* view, bool allowed);

// What the player asked of the engines and where the shots this game reports came down, since
// the last call, to send to the server.
std::vector<kcd2s::integration::SiegeUse> take_uses();
std::vector<kcd2s::integration::SiegeImpact> take_impacts();

// The world is gone: no engines, no stones, no hint.
void reset();

}  // namespace kcds::runtime::siege
