// Carrying another player's knocked-out body on the shoulder,
// WHGame 1.5.5: the game's own corpse carry.
//
// An actor's corpse carry is its + 392 (created, the getter RequestGrabCorpse and
// RequestPutCorpse use, C_ActorCarryCorpse): vtable + 56 picks up a body (its entity id), + 80
// puts it down; + 104 is its state (5 while it carries) and + 128 the carried body's entity id.
//
// The local player's carry: when its game picks a player's body up or lets it go, the client
// reports it (the server raises playerCarrying, refusable, and playerCarry / playerPutDown).
// Everybody else's: the server says who carries whom, and every other client has the carrier's
// replica pick up the body (the carried player's replica, or in the carried player's own game
// their own body), and put it down when the carry ends.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <functional>
#include <vector>

#include "kcd2s/integration/body_carry.hpp"

namespace kcds::runtime::carry {

using BodyLogger = void (*)(const char* format, ...);

// corpse_carry: (actor -> its corpse carry). False when it is missing.
bool install_body_carry(void* corpse_carry, BodyLogger logger);

// The entity whose body an actor carries, 0 for none (does not create the corpse carry).
std::uint32_t carried_entity(void* actor);

// Whether a player's body is held by a carry in this game, so that nothing else moves or
// animates it: the actor's corpse carry (the local player's) is picking it up or carrying it, or
// the server says someone carries this player.
bool body_held(std::uint32_t player, std::uint32_t entity, void* local_actor);

// Has an actor pick a body up with the game's own grab (the corpse carry's + 56).
void pick_up(void* actor, std::uint32_t entity);

// A player's actor and entity id in this game: the local player's own, or their replica's.
struct Body {
  void* actor = nullptr;
  std::uint32_t entity = 0;
};

struct Players {
  std::uint32_t local = 0;  // the local player's network id
  void* local_actor = nullptr;
  // A player's body in this game by network id; actor null when this game has none.
  std::function<Body(std::uint32_t player)> body_of;
  // The player whose body this entity is, 0 for none.
  std::function<std::uint32_t(std::uint32_t entity)> player_of;
};

// Takes the server's carries and orders, keeps them on this game's bodies, and returns what the
// local player's own carry did since the last call (to report).
std::vector<kcd2s::integration::BodyCarryReport> tick(
    const std::vector<kcd2s::integration::BodyCarry>& from_server, const Players& players);

}  // namespace kcds::runtime::carry
