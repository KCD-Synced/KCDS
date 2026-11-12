// What the KCD1 level (kcd1_rataje) is made of, file by file. Plain code, no game calls.
//
// KCD2 loads KCD1's compiled world as it is: its terrain/terrain.dat (version 28, which KCD2's
// terrain loader still reads beside its own 29, heights and surface ids in 16-bit words), its
// cover.ctc and indoor.dat, its merged meshes (terrain/merged_meshes_sectors) and its brushes
// (brush/*.cgf, named "%level%/Brush/..." in terrain.dat, so read from the level's own folder).
// terrain.dat holds the static objects (brushes and vegetation) with their models' paths.
//
// The level's description is KCD2's own (klaster's, as World Architect's custom maps take it),
// renamed, its layers, plants and the editor's object list left empty: KCD1's game data (whdata,
// newgame.whs, layers, waiting links, dog spots...) is KCD1's game and never reaches KCD2. Only
// its terrain surface types (the ground's detail materials) are KCD1's.
#pragma once

#include <string>

namespace kcds::runtime::kcd1 {

inline constexpr const char* kLevel = "kcd1_rataje";

// Whether a file of KCD1's Rataje level pack is part of the translated level (its name as the
// pack's directory writes it, either slash, any case).
bool level_file(const std::string& name);

// Whether a file of KCD1's GameData pack is one of the materials its world needs (materials/...).
// The rest of that pack (its scripts and tables, libs/) is KCD1's game and stays out.
bool material_file(const std::string& name);

// klaster's levelinfo.xml as kcd1_rataje's: its name, and KCD1's terrain information.
std::string level_info(const std::string& klaster_info, const std::string& kcd1_info);

// klaster's leveldata.xml as kcd1_rataje's: its name, no layers, no editor plant list, and KCD1's
// terrain surface types.
std::string level_data(const std::string& klaster_data, const std::string& kcd1_data);

// klaster's leveldataaction.xml and mission_mission0.xml with nothing of klaster's to load.
std::string level_action(const std::string& klaster_action);
std::string mission(const std::string& klaster_mission);

}  // namespace kcds::runtime::kcd1
