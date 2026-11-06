// The level's doors, kept in step with the server, WHGame 1.5.5.
//
// A door is an entity of the script class "AnimDoor" (Scripts/Entities/Doors/AnimDoor.lua). Its
// state lives in the entity's script table: bLocked, nDirection (-1 closed, 1 open), canBePicked,
// nUserId (set while the lock-picking minigame runs) and Properties.Lock (esLockTypes,
// bLockReversed, guidItemClassId, bLocked as built). Its native side is a C_AnimDoor component
// that is a lock (C_LockBase, the entity at + 56), found from the entity the way the game's lock
// script bindings find it.
//
//   report   The doors within kReportRadius of the player are read twice a second; new doors and
//            doors whose state changed are sent to the server (DOOR_REPORT). A change the
//            player made is told as DOOR_USE instead and not reported again.
//   apply    A DOOR_SET opens or closes the door with the game's own native OpenDoor and CloseDoor
//            (Force set; the same calls the quest scripts use), and locks or unlocks it with
//            C_LockBase::SetLocked (LockDoor and UnlockDoor). The side a door swings to is chosen
//            by the game from where the local player stands, so `side` is remembered and sent
//            back but not forced.
//   hang     A DOOR_SPAWN hangs a door a script made (Door.spawn): an AnimDoor entity
//            of the server's GUID (kcds_door_<guid>), its leaf, lock and physics in its
//            Properties as the level's doors have them, so from then on it is read, reported and
//            set like the level's own. A DOOR_SPAWN with `remove` takes it down.
//   use      A door within the player's reach that opened or closed, or whose lock opened (a key
//            or a lock pick), since the last read is the player's own work: it is told as
//            DOOR_USE (open, close, unlock, lockpick), which the server shares with the others.
//            Reported as a plain change, the server would put the door back. Nothing is hooked:
//            the script's UpdateBeforeAnimation binding calls slot 5 of an object whose vtable is
//            not C_AnimDoor's (slot 5 of that is a getter the linker shares with other classes).
//
// The states this module applies are known before the read sees them, so the read does not
// report them as the player's own. Engine thread.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/doors.hpp"

namespace kcds::runtime::doors {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* open_door = nullptr;          // OpenDoor(door, force)
  void* close_door = nullptr;         // the CloseDoor body (door, force, check busy)
  void* set_locked = nullptr;         // C_LockBase::SetLocked(lock, locked, context)
  void* lock_of_entity_id = nullptr;  // the lock component of an entity id
  void* (*entity_system)() = nullptr;
  bool (*send_report)(const std::vector<kcd2s::integration::DoorEntry>& doors) = nullptr;
  bool (*send_use)(const kcd2s::integration::DoorUse& use) = nullptr;
  bool (*send_reply)(const kcd2s::integration::DoorNearestReply& reply) = nullptr;
};

// False when a native is missing; the doors then stay the game's own.
bool install(const Natives& natives, Logger logger);

// Every frame in a ready world with the local player's entity and what the server sent since the
// last frame, oldest first.
void frame(void* local_entity, const std::vector<kcd2s::integration::DoorState>& sets,
           const std::vector<std::uint32_t>& nearest_requests,
           const std::vector<kcd2s::integration::DoorSpawn>& spawns);

// World Architect's door preview in its local mode (no server): whether the entity is a door,
// and whether it stands open and is locked.
struct PreviewState {
  bool door = false;
  bool open = false;
  bool locked = false;
};
PreviewState preview_state(void* entity);

// Opens or closes the door the way a DOOR_SET does (the game's own OpenDoor / CloseDoor, Force
// set; a locked door is unlocked before it opens). False when the entity is no door.
bool preview(void* entity, bool open);

// A world is left: what was reported is forgotten, so the next world reports its doors anew, and
// the doors scripts hung are taken down (the server hangs them again).
void reset();

// Where an NPC this game simulates stands now (issue 016): a door that opens or closes beside it
// is the NPC's work, told to the server as a use with the NPC (DOOR_USE npc), which every other
// game follows. Without it the door moved in the simulator's game only. Told as the NPC's state
// goes out; a body not told for kNpcForgetMs counts as gone. Engine thread.
void note_simulated_npc(std::uint32_t npc, float x, float y, float z);

}  // namespace kcds::runtime::doors
