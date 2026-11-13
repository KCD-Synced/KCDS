// KCD1's fires, candles and torches as KCD2 entities. Plain code, no game calls.
//
// KCD1 lights its houses, streets and taverns with ProceduralObject entities: each names a prefab
// of KCD1's prefab library (filePrefabLibrary "prefabs/fires.xml", ObjectVariation
// sPrefabVariation "Fires.Fireplaces.Candelabra_tall_five_candles"...) that KCD1 spawns in its
// place: the flames (ParticleEntity objects) and their light (an EntityArchetype of the Lights
// library). KCD2 has no ProceduralObject class, so the translator puts the prefab's flames and
// lights into the level itself, placed by the object's position and rotation; the candle holders,
// torches and fireplaces are brushes and already stand there.
//
// A light archetype is written out with KCD1's own properties (libs/entityarchetypes/lights.xml of
// KCD1's GameData pack, the same light units as KCD2's): KCD2's library lacks some of KCD1's
// archetypes and holds others with other values. The same goes for the level's own lights that
// name an archetype (Archetype="Lights.Standard.Light_Candles_five").
#pragma once

#include <cstddef>
#include <string>

namespace kcds::runtime::kcd1 {

// KCD1's prefab library and light archetypes, as their files hold them.
struct FireLibraries {
  std::string prefabs;     // prefabs/fires.xml
  std::string archetypes;  // libs/entityarchetypes/lights.xml
};

// The flames and lights of a ProceduralObject element of KCD1's level, as KCD2 entities with ids
// from `next_id` up; empty when its prefab is not in the library.
std::string procedural_object(const std::string& element, const FireLibraries& libraries,
                              long& next_id);

// A KCD1 entity naming a light archetype with that archetype's properties written in and the
// archetype's name left out; the element as it is when it names none the library has.
std::string with_archetype_properties(const std::string& element, const std::string& archetypes);

}  // namespace kcds::runtime::kcd1
