// The game's own NPCs and animals stay out of a multiplayer session until they are synced
// (spawning and movement,simulation ownership). Every client simulated
// its own villagers and animals, so each player saw different people and animals in different
// places, sitting on the seats the players wanted, and none of it was the same for anyone else;
// a player's swing hit a horse only it could see.
//
// Every entity is made by the entity system's SpawnEntity (CEntitySystem vtable slot 12,
// in WHGame 1.5.5), also the level's, the NPC simulation's and the animal spawners';
// the game client already hooks it (game_client.cpp hooked_spawn). SpawnEntity refuses a spawn
// itself by returning null (no class, the spawn lock), so the hook refuses the human NPC classes
// (NPC, NPC_Female, NPC_NAI) and the animal classes (Horse, Dog, Wolf, deer, sheep, cattle, ...)
// the same way: the class name, class vtable slot 2, of SEntitySpawnParams + 24.
//
// Our own entities go through: the players' replicas (Player, PlayerFemale) are not of a refused
// class, and the synced horses are created inside an OwnSpawn scope. The paper-doll dummies
// (InventoryDummyHorse, InventoryDummyDog, ...) are untouched.
//
// Any thread.
#pragma once

#include <cstdint>
#include <string>

namespace kcds::runtime::npcs {

using Logger = void (*)(const char* format, ...);

void install(Logger logger);

// True when SpawnEntity is to refuse the spawn (SEntitySpawnParams*): a human NPC or an animal
// the game spawned itself.
bool refuse_spawn(const void* params);

// The level's own people live in this world (WorldConfig level_people, issue 016): the level's
// spawns of the human NPC classes (NPC, NPC_Female) go through, and each one's entity is kept by
// its level GUID, as the placement catalog writes it (16 hex digits). Animals stay refused. Set
// before the world starts; false clears the kept entities.
void set_level_people(bool on);

// After the game spawned an entity (SEntitySpawnParams*, the entity): keeps a level person's.
void level_person_spawned(const void* params, void* entity);

// The entity id of the level person with that catalog GUID, 0 when this game has none.
std::uint32_t level_person_entity(const std::string& guid);

// Whether a spawn (SEntitySpawnParams*) is of the named entity class.
bool spawn_class_is(const void* params, const char* name);

// While one lives on a thread, its spawns are ours and never refused (a synced horse).
class OwnSpawn {
 public:
  OwnSpawn();
  ~OwnSpawn();
  OwnSpawn(const OwnSpawn&) = delete;
  OwnSpawn& operator=(const OwnSpawn&) = delete;
};

}  // namespace kcds::runtime::npcs
