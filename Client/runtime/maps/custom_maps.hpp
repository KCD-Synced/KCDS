// World Architect's custom maps:
// levels of the author's own, kcds_map_<name>, made by
// Dev/tools/architect/make_custom_map.py into %LOCALAPPDATA%\KCDS\maps\<level>\level.pak. The game
// folder is never written; the game loads them as its own levels:
//
//   - the level system lists a folder of Levels/ as a level when CryPak finds <folder>/level.pak
//     in it (CLevelSystem scan, FindFirst Levels/*.*, then IsFileExist): a small pack
//     of ours holds Levels/<level>/level.pak for every custom map, so the scan lists them;
//   - the level's load opens <level>/level.pak with CryPak's OpenPack (vtable + 56): for
//     a custom map our hook on that slot opens the map's real pack instead, bound at the level's
//     folder (OpenPack with a binding root, + 48), so its files are read as the level's.
//
// The maps on this PC are listed at startup, beside the mods' paks. A server running a custom map
// sends it to its players with a resource (maps/<level>/level.pak and map.json, Export for
// server): the resource runtime hands the files over in pieces (Client/resources/assets.cjs,
// { op: 'customMapFile', resource, map, name, offset, total, data }), they are written to the
// maps folder, and the engine thread lists the map and has the level system scan its levels
// again (CLevelSystem::Rescan, vtable slot 10), before the world loads.
//
// mount: the engine's thread at startup. handle_runtime_message: the network thread. frame and
// stage_player: engine thread. The rest: any thread.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::maps::custom {

using Logger = void (*)(const char* format, ...);

// Lists the custom maps found in the KCDS maps folder and keeps CryPak for maps that come later;
// from the mod pak mounting (branding/runtime_branding.cpp), with CryPak and its binding root.
void mount(void* pak, const char* binding_root, Logger logger);

// Whether a level name is a custom map's (kcds_map_ and 1 to 32 letters, digits or _).
bool is_custom(const std::string& level);

// Whether that custom map is on this PC.
bool exists(const std::string& level);

// Whether the game lists that custom map as a level (it can load it).
bool listed(const std::string& level);

// The custom maps the game lists (World Architect's map choice), sorted by name.
std::vector<std::string> names();

// Copies a custom map's level.pak and map.json into <folder>\<level>\ (an export); false when
// the map is not on this PC or a file could not be copied.
bool copy_to(const std::string& level, const std::filesystem::path& folder);

// A custom map's side in metres (its map.json), 0 when it is not on this PC.
int map_size(const std::string& level);

// Keeps a navmesh baked in the game (custom_navmesh.hpp) as the map's recast.next.pak: the game
// has the map's recast.pak open, so it takes the place of that one at the game's next start
// (mount). False when it could not be written.
bool keep_next_navmesh(const std::string& level, const std::vector<std::uint8_t>& index,
                       const std::vector<std::uint8_t>& tiles);

// True when the message was a custom map's file piece from a resource (it is taken).
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Lists the maps that came from a server since the last frame and has the level system scan its
// levels again; `framework` is CCryAction.
void frame(void* framework);

// Puts the local player in the middle of a custom map's ground, once per world (the map has no
// spawn point of its own): as the script's SetWorldPos, the entity's world matrix (slot 32) with a
// new translation, set through SetWorldTM (slot 30).
void stage_player(const std::string& level, void* local_entity);

}  // namespace kcds::runtime::maps::custom
