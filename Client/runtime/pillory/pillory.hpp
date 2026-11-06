// The local player in a pillory, put there and taken out by the server (Player.putInPillory and
// Player.releaseFromPillory, a KCDS addition), WHGame 1.5.5.
//
// The pillory is the game's stocks: a SmartObjectHolder entity "so_pillory" (prefab
// Prefabs/Pillory/pillory_v_2.xml) with a Lockpickable entity linked as "crime_pilloryLock". A
// player stands in it through the game's own unstance "Pillory", which the player-state handler
// (a singleton) starts and ends:
//
//   put in    S_Unstance u; init; name -> index (the holder's 64-bit entity
//             handle and "Pillory"); apply (handler, &u, instant 0, joined 1)
//   take out  an empty S_Unstance (id -1) applied the same way, as the game's own leave does
//   the lock  C_LockBase::SetLocked with bLock 0 is an unlock by a lock pick (the
//             minigame's end); a key's unlock (Lockpickable:Unlock) goes through the
//             lock script's ReportUnlocked instead. Either, on the lock of the pillory
//             the player is in, releases the player. The lock state is
//             not synced between games: a player who picks a pillory open does it in their own
//             game, so every unlock of a lock whose entity name starts with
//             "pillory_lockpickable" is reported to the server, which lets out whoever is in
//             the pillory of that lock.
//
// The holder is found by its name in the level (the entity system's FindEntityByName). The game's
// unstance code names it by a 64-bit handle; the form of that handle (the entity id, or the
// entity's GUID at + 16) is read off the local player once: the handler's player object keeps its
// own handle at + 8, and the form that equals it for the local player's entity is the one used.
//
// The pose is the owner's animation, which the other players' clients already replicate. A weapon
// in the player's hands is put away before the put-in, so nobody stands in the stocks with a drawn
// sword; the other players see it put away through the hands sync.
//
// Engine thread, except the hook of SetLocked, which only sets a flag.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/pillory.hpp"

namespace kcds::runtime::pillory {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* handler = nullptr;           // the player-state handler's accessor
  void* unstance_init = nullptr;     // S_Unstance's initializer
  void* unstance_by_name = nullptr;  // name and location handle -> S_Unstance
  void* unstance_apply = nullptr;    // starts or ends an unstance
  void* set_locked = nullptr;        // C_LockBase::SetLocked
  void* wuid_lookup = nullptr;       // an entity key's WUID in the WUID map
  void* wuid_map = nullptr;          // the WUID map's global
  void* services = nullptr;          // the game's services, for the action maps
  void* report_unlocked = nullptr;   // the lock script's ReportUnlocked (a key)
  void* script_lock = nullptr;       // the lock a lock script call names
  // Puts away what an actor holds (weapons::holster); optional.
  bool (*holster)(void* actor) = nullptr;
  // IEntitySystem, to find the nearest pillory (an order without a name); optional.
  void* (*entity_system)() = nullptr;
};

// Hooks SetLocked and ReportUnlocked. False when a native is missing.
bool install(const Natives& natives, Logger logger);

// Every frame in a world, with the local player's actor and entity and the server's orders since
// the last frame, oldest first. An order into the nearest pillory takes the holder (class
// SmartObjectHolder, name "so_pillory...") nearest to the player. Appends what to tell the server
// to `reports`: kPilloryLockpicked or kPilloryLeft for the player's own stay, kPilloryEntered
// with the holder's name once the player is in the nearest pillory, and kPilloryLockOpened with
// the lock's name for every pillory lock opened in this game (by this game's player, whether or
// not they are in a pillory: the lock state is not synchronised, so the one who picked it is the
// only game that sees it).
void frame(void* local_actor, void* local_entity,
           const std::vector<kcd2s::integration::PilloryOrder>& orders,
           std::vector<kcd2s::integration::PilloryReport>& reports);

// A world is left: the orders and the stay are forgotten (the server sends the pillory again when
// the next world is ready).
void reset();

// Whether the local player is in a pillory now. Engine thread.
bool in_stocks();

}  // namespace kcds::runtime::pillory
