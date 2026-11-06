// Containers the server holds, in the game (kcd2s/integration/stash.hpp).
//
// Every NetworkStash in view stands as the game's own corpse container, a StashCorpse entity
// ("kcds_stash_<id>"): its invisible cylinder takes the game's Loot prompt from any side, it has
// its own inventory and opens the game's transfer screen. A knocked-out player's body needs no
// more (the body lies there); any other container also shows the game's chest (chest_fancy_b) in
// its second slot, turned as the server says, and has the Stash class's Open prompt, only while
// the crosshair is on it (aim_chests). (The game's own Stash class shows the chest too,
// but a spawned one offered no prompt: the interaction did not find it.) What it holds comes from
// the server only for the player who opened it:
//
//   - the actor script binding's OpenItemTransferStore (binding, handler, the
//     container's entity id, its inventory id, filter, header), which the container's script
//     calls on use, is hooked: for one of ours the screen waits and the server is asked;
//   - the server's answer fills the container's inventory (found by its id through the item
//     services' inventory registry) with its rows, then the hook's original opens the
//     screen with the same arguments and a handler of our own (its slot 2 the local player's
//     entity id, slot 11 the end of the call);
//   - the screen's per-item transfer ({controller, target inventory}, item, units) is
//     hooked: an item of the container (its inventory at item + 0x90) or one of the player's
//     server rows going into it stays where it is and is reported (take, put);
//   - the screen's close callback (the binding, which keeps the container's entity id
//     at + 128 until then) reports the close. A use the server refused runs that callback for the
//     container too, so its script, which took itself for open, is closed again.
//
// A script's lock (Stash#locked, canBePicked, keyClass) rides on the container's
// fields and is put on the game's own lock: StashCorpse's native side is a C_StashCorpse, a
// C_LockBase, found from the entity by the lock lookup the lock script bindings make.
// Its Properties.Lock.bCanLockPick and guidItemClassId are set in the script table, then
// C_LockBase::SetLocked, which calls the script's own Lock or Unlock. The container's
// script (Stash.OnUsed, StashCorpse:GetActions) then offers no Search while it is locked, the
// game's lock-picking hold where canBePicked, and opens for the key's holder.
//
// A lock the player opens in their game is reported to the server (STASH_UNLOCK), which unlocks
// the container for everyone and then grants the open:
//
//   - a lock pick: the lock-picking minigame's unlock (the minigame, success, ...; the
//     entity id of the lock it works at + 40), which calls SetLocked(lock, 0) itself, is hooked;
//     after it, an unlock of one of ours the server holds locked is reported as `Pick`;
//   - the key: the container's script lets the key's holder use a locked container, so a use the
//     open_transfer hook sees on one the server holds locked, with no pick reported, is the key;
//     the unlock is reported before the open.
//
// A use the server refused on a container it holds locked puts the server's lock on the game's
// again, so a refused pick or key does not leave it open in this game.
//
// A level chest the server adopted (Stash.adopt) is the level's own Stash entity, found
// by its level GUID once the level has streamed it in: nothing is spawned or moved, the same hooks
// serve its use (its inventory shows the server's rows) and its lock, and it stays in the level
// when the server gives it back (with the lock and what it showed last).
//
// Engine thread.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/stash.hpp"

namespace kcds::runtime::stash {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*services)() = nullptr;
  void* (*local_entity)() = nullptr;
  void* open_transfer = nullptr;    // hooked
  void* transfer_closed = nullptr;  // hooked
  void* transfer_item = nullptr;    // hooked
  void* inventory_by_id = nullptr;  // (the inventory registry, &inventory id)
  // The lock; without them a container stays unlocked.
  void* set_locked = nullptr;  // C_LockBase::SetLocked(lock, locked, context)
  // The lock component of an entity id: it reads the id at + 0x28 of its argument
  // (itself takes the game object of the entity, not the CEntity).
  void* lock_of_entity_id = nullptr;
  // The lock-picking minigame's unlock, hooked; without it a pick is not reported.
  void* lockpick_unlock = nullptr;
};

// A script's lock on a container.
struct Lock {
  bool locked = false;
  bool canBePicked = true;
  std::string key_class;  // the key's item class GUID, empty for none
  // The lock-picking difficulty a script set, 0 to 1; below 0 the game's own.
  float difficulty = -1.0f;
  // Which lock the lock-picking minigame shows: 0 the level's, 1 common, 2 fancy.
  int fanciness = 0;

  friend bool operator==(const Lock& a, const Lock& b) {
    return a.locked == b.locked && a.canBePicked == b.canBePicked && a.key_class == b.key_class &&
           a.difficulty == b.difficulty && a.fanciness == b.fanciness;
  }
  friend bool operator!=(const Lock& a, const Lock& b) {
    return !(a == b);
  }
};

bool install(const Natives& natives, Logger logger);

// A container in view: its network id and where it stands.
struct Seen {
  std::uint32_t id = 0;
  float position[3] = {};
  float rotation[4] = {0, 0, 0, 1};  // x, y, z, w
  bool body = false;                 // a knocked-out player's or a dead NPC's body
  // A dead NPC's body: the body's own entity, whose Rob opens this container instead
  // of the body's own inventory, and its actor, whose clothes stay on it.
  std::uint32_t body_entity = 0;
  void* wearer = nullptr;
  // Its lock; a body has none.
  Lock lock;
  // An adopted level chest's level GUID (sixteen hex digits), empty for any other container.
  std::string level_guid;
  // The mesh a script built it as (Stash.spawn's model): a .cgf, .cga or .cdf path,
  // empty for the default chest.
  std::string model;
  // Whether the server's stock of it holds nothing, what its prompt says.
  bool empty = true;
};

// Stands the containers in view and takes away the rest.
void sync(const std::vector<Seen>& seen);

// The entities of the server's NPC bodies: the game's own Rob opens none of them,
// except a dead one's container, so nothing leaves a body but through the server.
void set_server_bodies(std::vector<std::uint32_t> entities);

// Every frame: a script's chest offers its prompt only while the crosshair is on it (the view
// camera's 3x4 world matrix, null when unknown).
void aim_chests(const float* camera);

// What the server sent for a container the local player holds.
void show(const kcd2s::integration::StashShow& show);

// What the local player did with containers since the last call, oldest first: open, close,
// take, put, or (with `unlock`) opened a container's lock, which goes as STASH_UNLOCK.
struct Report {
  kcd2s::integration::InventoryReport::Kind kind =
      kcd2s::integration::InventoryReport::Kind::StashOpen;
  std::uint32_t stash = 0;
  std::string row;
  std::uint32_t units = 0;
  bool unlock = false;
  kcd2s::integration::StashUnlockMethod method = kcd2s::integration::StashUnlockMethod::Pick;
};
std::vector<Report> take_reports();

// The connection ended: every container goes.
void reset();

}  // namespace kcds::runtime::stash
