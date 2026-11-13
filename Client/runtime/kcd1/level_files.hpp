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

#include <cstdint>
#include <string>
#include <vector>

namespace kcds::runtime::kcd1 {

inline constexpr const char* kLevel = "kcd1_rataje";

// Whether a file of KCD1's Rataje level pack is part of the translated level (its name as the
// pack's directory writes it, either slash, any case).
bool level_file(const std::string& name);

// The state each of KCD1's layers (leveldata.xml's list) starts in, one line per layer:
// "1 <id> <name>" on, "0 <id> <name>" off. KCD1 switches its layers by game profiles (whdata_1: a
// farm burnt or not, Talmberk's fort, the Vranik camp, Pribyslavitz's buildings...); a layer is on
// when one of the profiles a new game starts with holds it (not burnt, as it was, the first
// state, the building sites, the ambient life without rain), or when no profile holds it and it
// is no part of KCD1's story (cutscenes, quests, events, battles, later states) or rain. The
// client puts the layers' objects (terrain.dat's, by their layer id) in these states when the
// world is ready, and a server switches them from there (LevelEdit); a layer's entities come and
// go with it (on_layer).
std::string layer_starts(const std::string& kcd1_whdata, const std::string& kcd1_leveldata);

// A layer's translated entities (an <Objects> list) as KCD2 writes a layer file,
// layers/<name>.xml: every entity names its layer (Layer="<name>"), and the entity system loads
// them when the layer is switched on.
std::string on_layer(const std::string& objects, const std::string& layer);

// Whether a file of KCD1's GameData pack is one of the materials its world needs (materials/...).
// The rest of that pack (its scripts and tables, libs/) is KCD1's game and stays out.
bool material_file(const std::string& name);

// Whether a file of KCD1's Textures packs is one of its world's textures (structures, terrain,
// rock, cube maps, particles, details, decals...). The engine's own folders (defaults, clouds,
// skies, lights, water, ui...) stay out: KCD2 asks for some files there that it does not have,
// and KCD1's (8-bit, ATI2 and DXT3 textures the engine no longer creates) would be found instead.
bool texture_file(const std::string& name);

// Whether a file of KCD1's Characters pack is one of its world's animated props (doors, gates,
// carts..., objects/characters/assets/). Its people stay out. A door's skeleton
// (anim_door_left.chr...) is also KCD2's, whose own is found first, so KCD1's doors move with
// KCD2's door animations.
bool prop_file(const std::string& name);

// KCD1's texture headers: <name>.dds ("DDS " and the header, the smallest mips after it) and the
// attached alpha's <name>.dds.a (the header alone). The larger mips lie in <name>.dds.1, .2...
// (the alpha's in .1a, .2a...). KCD2 reads how many mips the file itself holds from the header
// (its persistent mip count, the byte at + 116 of a .dds), which KCD1 leaves 0, and the texture
// then has no format when it is streamed. The translator sets it: mips less the split files.
bool texture_header(const std::string& name);

// The name of a header's k-th split file (1 = the largest mip): "<name>.k", or for an attached
// alpha "<name without .a>.ka".
std::string split_file(const std::string& header_name, int k);

// A KCD1 material (.mtl) as KCD2's shaders read it; the text unchanged when nothing needs it.
//   - The second UV overlay (%SNDUVS, a building's dirt and wear multiplied over its tiling
//     textures) is the same effect in both games, but KCD1 keeps its texture in the "[1] Diffuse"
//     slot and KCD2 reads it from the Opacity slot, where KCD1 keeps a mask (which KCD2 would
//     smear over the walls in blotches). The overlay moves to the Opacity slot; a material with
//     the flag but no overlay loses the flag.
//   - The numeric GenMask is left out: its bits differ between the games' shaders, and KCD2 then
//     takes the flags from StringGenMask's names.
std::string translate_material(const std::string& text);

// Sets the persistent mip count of a header (`attached_alpha`: a .dds.a) that has `split_files`
// split files; false when the bytes are no such header or it needs no change.
bool fix_persistent_mips(std::vector<std::uint8_t>& header, bool attached_alpha, int split_files);

// klaster's levelinfo.xml as kcd1_rataje's: its name, and KCD1's terrain information.
std::string level_info(const std::string& klaster_info, const std::string& kcd1_info);

// klaster's leveldata.xml as kcd1_rataje's: its name, no layers, no editor plant list, and KCD1's
// terrain surface types.
std::string level_data(const std::string& klaster_data, const std::string& kcd1_data);

// klaster's leveldataaction.xml and mission_mission0.xml with nothing of klaster's to load.
std::string level_action(const std::string& klaster_action);
std::string mission(const std::string& klaster_mission);

}  // namespace kcds::runtime::kcd1
