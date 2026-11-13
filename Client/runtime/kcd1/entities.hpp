// KCD1's entities (objects_mission0.xml) as KCD2's: the things a player uses run on KCD2's own
// entity classes. Plain code, no game calls.
//
// KCD1's level holds every entity of its map in objects_mission0.xml, one element per line block
// at the top level ("  <Entity ...>" ... "  </Entity>", "  <Object .../>"). Kept and translated:
//   - AnimDoor: KCD2's door. It needs its animation set (esDoorAnimSet: VillageLeft or
//     VillageRight, from KCD1's DoorLeft / DoorRight helpers) and KCD2's door smart object type;
//     KCD1's hours the door stays open (fKeepOpenFrom / fKeepOpenUntil) are KCD2's lock's
//     fKeepUnlockedFrom / fKeepUnlockedTo.
//   - Stash: KCD2's chest, sack, shelf... KCD1 names its inventory by a database id of KCD1's;
//     the translated stash generates one of KCD2's inventories by the kind of its model.
//   - Ladder: KCD2's helper per height (Ladder_225 ... Ladder_400) and smart object type.
//   - Chair, and the world's lights, light probes and particle effects (Light, ReflexLight,
//     EnvironmentLight, ParticleEffect: fires, smoke...): as they are, the same classes and
//     properties in both games.
//   - Bed: KCD2 has no Bed class. A bed is three entities there: the model (GeomEntity), the
//     place the sleeper lies on (StanceSmartObject, by the kind of bed) and the trigger the player
//     uses (BedTrigger, "Sleep and save").
//   - Dice tables: KCD2's dice table entities at each of KCD1's (dice_tables.hpp).
//   - ProceduralObject (fires, candles, torches): the flames and lights of its prefab
//     (procedural_objects.hpp); a Light naming an archetype gets KCD1's archetype's properties.
// KCD2 writes the smart object helpers' attribute as soclass_SmartObjectHelpers (KCD1:
// soclasses_...) and an entity's 64-bit GUID as xxxxxxxx-xxxx-xxxx (KCD1: 16 hex digits).
// Everything else (KCD1's people, quests, areas, sounds) is left out. KCD1's decals are in
// terrain.dat's objects (terrain_convert.hpp).
#pragma once

#include <cstddef>
#include <map>
#include <string>

#include "procedural_objects.hpp"

namespace kcds::runtime::kcd1 {

struct EntityCounts {
  std::map<std::string, std::size_t> kept;  // by KCD1 class
  std::size_t left_out = 0;
  // The id of the next entity the translation adds (beds, fires, dice tables): above every id of
  // KCD1's level and layers (the highest is below 130000), counted on through all of its files,
  // and below 262142: the entity system keeps an id's index in 18 bits, and a larger id would
  // take the slot of a smaller one.
  long next_id = 140000;
};

// What the translation reads besides KCD1's level.
struct ObjectSources {
  // The KCD2 level's (klaster's) objects_mission0.xml, whose dice table KCD1's dice tables are
  // made of (dice_tables.hpp).
  std::string kcd2_objects;
  // KCD1's fire prefabs and light archetypes (procedural_objects.hpp).
  FireLibraries fires;
};

// KCD2's objects_mission0.xml for KCD1's.
std::string translate_objects(const std::string& kcd1_objects, const ObjectSources& sources,
                              EntityCounts& counts);

}  // namespace kcds::runtime::kcd1
