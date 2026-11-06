// A seat a player sits on is taken on every client.
//
// The game's seats, beds and the other places an actor uses are smart objects (StanceSmartObject
// and the like, WHGame 1.5.5):
//
//   smart object + 8     its WUID; the level's smart objects have the same WUID on every client
//   smart object + 240   its reservation table: per resource the WUIDs of the actors holding it
//                        (made with the first reservation, freed with the last)
//   smart object + 296   its capacity per resource (a vector of int, -1 unlimited)
//
// ActionTrigger:IsUsable asks XGenAIModule.IsStanceAvailable, true while a resource's
// capacity is more than the actors holding it; a seat that is taken shows no "Sit"
// and E does nothing. The player sitting down reserves its seat for itself (reserve
// (smart object, WUID, resource), only while it is available) and standing up releases it
//
// A replica does not sit through the game's AI, so on the other clients its seat stayed free and
// another player could sit into it (two clients). The owner's client sees the
// reservations its player makes (the reserve hook) and publishes the ones the player still holds
// in the NetworkPlayer field "seats"; every other client reserves the same smart objects for the
// replica and releases them when they leave the field or the replica goes. The field is state, so
// a client that joins later takes the seats as well.
//
// An actor's WUID is its AI object's (+ 8), which the NPC manager keeps per entity id (records
// sorted by entity id at manager + 48: entity id, actor, AI object; the local player's AI object
// at + 104). The local player's WUID is the same number on every client, so a replica holds its
// seats with its own AI object's WUID, never its owner's.
//
// Engine thread, except the reserve hook (the thread the game reserves on).
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::seats {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* reserve = nullptr;             // a smart object's reservation, hooked
  void* release = nullptr;             // its release
  void* holds = nullptr;               // whether a WUID holds a resource of a table
  void* find_smart_object = nullptr;   // the smart object of a WUID
  void* const* npc_manager = nullptr;  // the NPC manager
  void* find_npc = nullptr;            // its record of an entity id
};

// False when a native is missing or the hook fails.
bool install(const Natives& natives, Logger logger);

// When (GetTickCount64) the game last gave the entity's NPC a seat, 0 for not since the last reset.
std::uint64_t seat_taken_at(std::uint32_t entity_id);

// Whether the entity's actor holds a seat or another smart object it uses now (a bench, a bed, a
// workbench), as the game reserved it in this game (issue 016: an NPC sitting on a bench). Engine
// thread.
bool holds_seat(std::uint32_t entity_id);

// The reservations the local player holds, as the field "seats": "WUID:resource" (the WUID in 16
// hex digits) joined by ';', "" for none.
std::string local_seats();

// Keeps the replica of player (its entity) holding the seats of its owner's field.
void keep_replica(std::uint32_t player, std::uint32_t entity_id, const std::string& field);

// The replica of player goes: its seats are released.
void forget(std::uint32_t player);

// The replicas of this world go.
void reset();

}  // namespace kcds::runtime::seats
