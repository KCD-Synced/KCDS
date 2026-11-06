// The game's player horse of the local player: a horse the server gives the local
// player is that player's own horse in their game, so mounting it is a plain mount, not a theft.
//
// The game keeps one horse companion per player (Pebbles in the campaign). The horse's Lua
// actions offer "Mount" on a short press for it and "Mount and steal", held, for any other horse
// (Scripts.pak Horse.lua GetActions: player:GetHorseId() == self.id, else IsMountLegal()). The
// game's own setter (the console command wh_horse_StealCurrentHorse, "Makes the
// horse the player sits on its primary horse companion", and the script binds
// Player.SetPlayerHorse / ClearPlayerHorse call it) takes the local player's horse component
// (C_Player + 3432) and the horse's entity id, or 0 to let the horse go:
//
// - it lets the player's previous horse go (the owner of its inventory back to nobody) and, when
//   the previous horse is another one, moves that horse's equipped items onto the new one;
// - it links the horse as the player's horse companion (the horse's + 1640 part takes
//   the player as its master and the player's faction data), makes the player the owner of the
//   horse's inventory (the services' inventory records), and tells the
//   player's AI "horseModule:onHorsePossessed".
//
// The getter (companion kind 0 of an actor) is what player:GetHorseId() reads.
// Other players' games do not run this for the horse: there it stays someone else's horse, and
// taking it is the game's own steal. Engine thread only.
#ifndef KCDS_RUNTIME_RIDING_PLAYER_HORSE_HPP
#define KCDS_RUNTIME_RIDING_PLAYER_HORSE_HPP

#include <cstdint>

namespace kcds::runtime::riding {

using Logger = void (*)(const char* format, ...);

struct PlayerHorseNatives {
  void* set_player_horse = nullptr;  // (horse component, horse entity id or 0)
  void* companion_of = nullptr;      // (actor, companion kind) -> its actor
};

// False when one of them is missing; the calls below then do nothing.
bool init_player_horses(const PlayerHorseNatives& natives, Logger logger);

// Whether the horse's body has the part the game links a companion by (its + 1640).
bool horse_ready(void* horse_actor);

// Makes the horse the local player's own horse in the game, after letting any other horse the
// game held as theirs go. True when the game took it (or already held it).
bool make_player_horse(void* local_actor, void* horse_actor, std::uint32_t horse_entity);

// The local player's game holds no player horse any more.
void clear_player_horse(void* local_actor);

// Called before the horse's body leaves the game: when it is the local player's horse there, the
// game lets it go first, so no player horse names a removed body. True when it was.
bool release_player_horse(void* local_actor, void* horse_actor);

}  // namespace kcds::runtime::riding

#endif  // KCDS_RUNTIME_RIDING_PLAYER_HORSE_HPP
