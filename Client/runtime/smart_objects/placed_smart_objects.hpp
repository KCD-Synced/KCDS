// The smart objects scripts place, WHGame 1.5.5.
//
// Every prefab of the game (Prefabs/*.xml) is a runtime template of its PrefabTemplateLibrary
// (loads them all), and the game builds one whole at an anchor entity: its furniture,
// solids and the entities its interaction runs on, links and all. That is the script binding
// Game.SpawnPrefab(entityId, prefabTemplateId, maxSpawn), which parses the template's
// GUID and calls the prefab manager's spawn (manager, &template, anchor entity id,
// max spawn, now). The manager keeps one instance per anchor entity id (a std::map at manager +
// 8: key + 32, instance + 40); an instance holds the ids of the entities it built (+ 64 to + 72),
// its brushes (+ 88 to + 96) and its child instances (+ 112 to + 120), and takes all of
// them down.
//
// A SMART_OBJECT order places one: a TagPoint of its own (kcds_so_<id>) where the server says,
// then the template built at it. Its removal takes the instance down and removes the anchor.
//
// The template gives its entities EntityGuid 0, so an instance's entities would
// be keyed differently on every client. While the manager builds ours, the entity system's spawn
// from a template record (IEntitySystem slot 38, which calls with the record's spawn
// params) is given a GUID made from the order's id and the entity's place in the build, the same
// on every client: kcso in the high 16 bits, the id, then the place (smart_object_guid). The
// game's own keys of the level's entities come from their GUIDs too.
//
// Engine thread.
#pragma once

#include <cstdint>
#include <vector>

#include "kcd2s/integration/smart_objects.hpp"

namespace kcds::runtime::smart_objects {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* spawn_prefab = nullptr;          // the prefab manager's spawn
  void* clear_instance = nullptr;        // an instance taken down
  void* const* prefab_system = nullptr;  // + 0x150 the prefab manager
  void* entity_system = nullptr;         // IEntitySystem, whose slot 38 is hooked
};

// The EntityGuid of the entity at `place` of the build of smart object `id`.
std::uint64_t smart_object_guid(std::uint32_t id, std::uint32_t place);

// False when a native is missing; smart objects are then not built.
bool install(const Natives& natives, Logger logger);

// The server's orders since the last frame, oldest first.
void apply(const std::vector<kcd2s::integration::SmartObjectOrder>& orders);

// A world is left: every smart object placed is taken down (the server places them again).
void reset();

}  // namespace kcds::runtime::smart_objects
