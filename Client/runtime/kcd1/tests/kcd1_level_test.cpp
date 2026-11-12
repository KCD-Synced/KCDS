// The KCD1 map's level files and packs: what is taken from KCD1, what from KCD2, and packs that
// read back entry for entry.
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../../files/stored_zip.hpp"
#include "../../files/zip_read.hpp"
#include "../entities.hpp"
#include "../install.hpp"
#include "../level_files.hpp"
#include "../pack_writer.hpp"
#include "../terrain_convert.hpp"
#include "kcd2s/testkit.hpp"

namespace kcd1 = kcds::runtime::kcd1;
namespace files = kcds::runtime::files;

namespace {

bool has(const std::string& text, const std::string& part) {
  return text.find(part) != std::string::npos;
}

std::filesystem::path temporary(const char* name) {
  return std::filesystem::temp_directory_path() / name;
}

}  // namespace

KCD2S_TEST(kcd1_level, the_compiled_world_comes_from_kcd1_and_its_game_data_stays_out) {
  KCD2S_CHECK(kcd1::level_file("terrain\\terrain.dat"));
  KCD2S_CHECK(kcd1::level_file("Terrain/Cover.ctc"));
  KCD2S_CHECK(kcd1::level_file("terrain/indoor.dat"));
  KCD2S_CHECK(kcd1::level_file("terrain\\merged_meshes_sectors\\sector_0_0_9_1.dat"));
  KCD2S_CHECK(kcd1::level_file("Brush\\designer_755.cgf"));
  KCD2S_CHECK(!kcd1::level_file("whdata_0"));
  KCD2S_CHECK(!kcd1::level_file("newgame.whs"));
  KCD2S_CHECK(!kcd1::level_file("objects_mission0.xml"));
  KCD2S_CHECK(!kcd1::level_file("layers/_other_bakery.xml"));
  KCD2S_CHECK(!kcd1::level_file("leveldata.xml"));
  KCD2S_CHECK(kcd1::material_file("Materials\\terrain\\grass\\grass_pasture.mtl"));
  KCD2S_CHECK(!kcd1::material_file("libs/tables/item/item.xml"));
  KCD2S_CHECK(!kcd1::material_file("prefabs/houses.xml"));
  // The world's textures come, the engine's own folders stay KCD2's.
  KCD2S_CHECK(kcd1::texture_file("Textures\\structures\\wicker\\wicker_02_ddn.dds"));
  KCD2S_CHECK(kcd1::texture_file("textures/terrain/grass/grass_02_blend_displ.dds.1"));
  KCD2S_CHECK(kcd1::texture_file("textures/cubemaps/rataje/nn_forest_probe_3/04_40_cm.dds"));
  KCD2S_CHECK(!kcd1::texture_file("textures/defaults/rain/puddle0.dds"));
  KCD2S_CHECK(!kcd1::texture_file("textures/defaults/noise3d.dds"));
  KCD2S_CHECK(!kcd1::texture_file("textures/clouds/lyr1/lyr1_1x1_01_diff.dds"));
  KCD2S_CHECK(!kcd1::texture_file("textures/skys/night/full_moon.dds"));
  KCD2S_CHECK(!kcd1::texture_file("textures/default_mat_warning.dds"));
  KCD2S_CHECK(!kcd1::texture_file("objects/buildings/barricade.cgf"));
}

KCD2S_TEST(kcd1_level, the_description_is_kcd2s_renamed_with_kcd1s_terrain) {
  const std::string klaster_info =
      "<LevelInfo Name=\"data/levels/klaster\" HeightmapSize=\"800\">\n"
      "\t<TerrainInfo HeightmapSize=\"2048\" UnitSize=\"1\" SectorsTableSize=\"32\" />\n"
      "\t<Missions>\n\t\t<Mission Name=\"Mission0\" Description=\"\" />\n\t</Missions>\n"
      "</LevelInfo>\n";
  const std::string kcd1_info =
      "<LevelInfo Name=\"data/levels/rataje\" HeightmapSize=\"1000\">\n"
      "  <TerrainInfo HeightmapSize=\"4096\" UnitSize=\"1\" SectorsTableSize=\"64\" />\n"
      "</LevelInfo>\n";
  const std::string info = kcd1::level_info(klaster_info, kcd1_info);
  KCD2S_CHECK(has(info, "Name=\"data/levels/kcd1_rataje\""));
  KCD2S_CHECK(has(info, "<TerrainInfo HeightmapSize=\"4096\" UnitSize=\"1\""));
  KCD2S_CHECK(!has(info, "2048"));
  KCD2S_CHECK(has(info, "<Mission Name=\"Mission0\""));

  const std::string klaster_data =
      "<LevelData>\n"
      "\t<LevelInfo Name=\"klaster\" HeightmapSize=\"800\" />\n"
      "\t<SurfaceTypes>\n\t\t<SurfaceType Name=\"Materials/material_terrain_default\" />\n"
      "\t</SurfaceTypes>\n"
      "\t<CollisionClasses>kept</CollisionClasses>\n"
      "\t<Layers>\n\t\t<Layer Name=\"_enviro\" />\n\t</Layers>\n"
      "\t<Vegetation>\n\t\t<Object Size=\"0.7\" />\n\t</Vegetation>\n"
      "</LevelData>\n";
  const std::string kcd1_data =
      "<LevelData>\n"
      "  <LevelInfo Name=\"rataje\" HeightmapSize=\"1000\" />\n"
      "  <SurfaceTypes>\n    <SurfaceType Name=\"Materials/terrain/grass/grass_pasture\" />\n"
      "  </SurfaceTypes>\n"
      "  <Layers>\n    <Layer Name=\"kcd1\" />\n  </Layers>\n"
      "</LevelData>\n";
  const std::string data = kcd1::level_data(klaster_data, kcd1_data);
  KCD2S_CHECK(has(data, "<LevelInfo Name=\"kcd1_rataje\" HeightmapSize=\"1000\" />"));
  KCD2S_CHECK(has(data, "grass_pasture"));
  KCD2S_CHECK(!has(data, "material_terrain_default"));
  KCD2S_CHECK(has(data, "<Layers />"));
  KCD2S_CHECK(!has(data, "_enviro"));
  KCD2S_CHECK(!has(data, "kcd1\" />"));
  KCD2S_CHECK(has(data, "<Vegetation />"));
  KCD2S_CHECK(has(data, "<CollisionClasses>kept</CollisionClasses>"));

  const std::string action =
      "<Mission Name=\"Mission0\" CGFCount=\"3789\" ProgressBarRange=\"3789\" />";
  KCD2S_CHECK_EQ(
      kcd1::level_action(action),
      std::string("<Mission Name=\"Mission0\" CGFCount=\"0\" ProgressBarRange=\"1\" />"));
  KCD2S_CHECK_EQ(kcd1::mission("<Mission CGFCount=\"12\"><Environment /></Mission>"),
                 std::string("<Mission CGFCount=\"0\"><Environment /></Mission>"));
}

KCD2S_TEST(kcd1_level, a_kcd1_material_is_written_as_kcd2s_shaders_read_it) {
  // The second UV overlay moves to the Opacity slot; the numeric mask goes.
  const std::string roof =
      "<Material MtlFlags=\"524320\" Shader=\"Illum\" GenMask=\"1000020080000000\" "
      "StringGenMask=\"%NORMAL_MAP%SNDUVS%SUBSURFACE_SCATTERING\">\n"
      "  <Textures>\n"
      "    <Texture Map=\"Diffuse\" File=\"Textures/structures/roof/roof_01_diff.tif\" />\n"
      "    <Texture Map=\"[1] Diffuse\" File=\"objects/buildings/gate02_2uv_diff.tif\" />\n"
      "  </Textures>\n"
      "</Material>\n";
  const std::string translated = kcd1::translate_material(roof);
  KCD2S_CHECK(!has(translated, "gate02_2uv_diff"));
  KCD2S_CHECK(!has(translated, "[1] Diffuse"));
  KCD2S_CHECK(
      has(translated, "Map=\"Diffuse\" File=\"Textures/structures/roof/roof_01_diff.tif\""));
  KCD2S_CHECK(!has(translated, "GenMask=\"1000020080000000\""));
  KCD2S_CHECK(has(translated, "StringGenMask=\"%NORMAL_MAP%SUBSURFACE_SCATTERING\""));

  // Sub-materials each lose their overlay; an Opacity texture stays.
  const std::string multi =
      "<Material MtlFlags=\"0\">\n <SubMaterials>\n"
      "  <Material Name=\"a\" GenMask=\"1\" StringGenMask=\"%SNDUVS\">\n"
      "   <Textures><Texture Map=\"Opacity\" File=\"a_op.tif\" />"
      "<Texture Map=\"[1] Diffuse\" File=\"a_2uv.tif\" /></Textures>\n"
      "  </Material>\n"
      "  <Material Name=\"b\" StringGenMask=\"%DETAIL_MAPPING%SNDUVS\">\n"
      "   <Textures><Texture Map=\"[1] Diffuse\" File=\"b_2uv.tif\" /></Textures>\n"
      "  </Material>\n"
      " </SubMaterials>\n</Material>\n";
  const std::string both = kcd1::translate_material(multi);
  KCD2S_CHECK(has(both, "<Material Name=\"a\" StringGenMask=\"\">"));
  KCD2S_CHECK(has(both, "Map=\"Opacity\" File=\"a_op.tif\""));
  KCD2S_CHECK(!has(both, "a_2uv.tif"));
  KCD2S_CHECK(has(both, "<Material Name=\"b\" StringGenMask=\"%DETAIL_MAPPING\">"));
  KCD2S_CHECK(!has(both, "b_2uv.tif"));
  KCD2S_CHECK(has(both, "</SubMaterials>"));

  // Nothing to change: the text as it was.
  const std::string plain = "<Material Shader=\"Illum\" StringGenMask=\"%NORMAL_MAP\" />";
  KCD2S_CHECK_EQ(kcd1::translate_material(plain), plain);
}

KCD2S_TEST(kcd1_level, a_texture_header_gets_the_mips_its_file_holds) {
  KCD2S_CHECK(kcd1::texture_header("Objects\\flag\\flag_diff.dds"));
  KCD2S_CHECK(kcd1::texture_header("objects/barricade/barricade_ddna.dds.a"));
  KCD2S_CHECK(!kcd1::texture_header("objects/barricade/barricade_ddna.dds.1"));
  KCD2S_CHECK(!kcd1::texture_header("objects/barricade/barricade_ddna.dds.1a"));
  KCD2S_CHECK(!kcd1::texture_header("objects/barricade/barricade.mtl"));
  KCD2S_CHECK_EQ(kcd1::split_file("objects/b/b_diff.dds", 2),
                 std::string("objects/b/b_diff.dds.2"));
  KCD2S_CHECK_EQ(kcd1::split_file("objects/b/b_ddna.dds.a", 1),
                 std::string("objects/b/b_ddna.dds.1a"));

  // KCD1's header: 8 mips, 5 of them split off, the persistent count 0.
  std::vector<std::uint8_t> dds(4 + 124 + 16, 0);
  dds[0] = 'D';
  dds[1] = 'D';
  dds[2] = 'S';
  dds[3] = ' ';
  dds[4] = 124;
  dds[4 + 24] = 8;
  KCD2S_CHECK(kcd1::fix_persistent_mips(dds, false, 5));
  KCD2S_CHECK_EQ(int(dds[116]), 3);
  // Already set: no change.
  KCD2S_CHECK(!kcd1::fix_persistent_mips(dds, false, 5));
  // The attached alpha's header has no "DDS ".
  std::vector<std::uint8_t> alpha(124 + 8, 0);
  alpha[0] = 124;
  alpha[24] = 9;
  KCD2S_CHECK(kcd1::fix_persistent_mips(alpha, true, 5));
  KCD2S_CHECK_EQ(int(alpha[112]), 4);
  // Not a header, or every mip split off: untouched.
  std::vector<std::uint8_t> other(140, 0);
  KCD2S_CHECK(!kcd1::fix_persistent_mips(other, false, 1));
  std::vector<std::uint8_t> all_split = dds;
  all_split[116] = 0;
  KCD2S_CHECK(!kcd1::fix_persistent_mips(all_split, false, 8));
  KCD2S_CHECK_EQ(int(all_split[116]), 0);
}

namespace {

void put32(std::vector<std::uint8_t>& out, std::size_t at, std::uint32_t value) {
  std::memcpy(out.data() + at, &value, 4);
}

void put16(std::vector<std::uint8_t>& out, std::size_t at, std::uint16_t value) {
  std::memcpy(out.data() + at, &value, 2);
}

float float_at(const std::vector<std::uint8_t>& data, std::size_t at) {
  float value = 0.0f;
  std::memcpy(&value, data.data() + at, 4);
  return value;
}

}  // namespace

KCD2S_TEST(kcd1_level, kcd1_brushes_and_plants_become_version_8_records) {
  // A KCD1 brush: type 1, then 96 bytes.
  std::vector<std::uint8_t> brush(4 + 96, 0);
  put32(brush, 0, 1);
  put16(brush, 4 + 24, 3);            // layer
  put32(brush, 4 + 28, 0x60000008u);  // render flags
  put16(brush, 4 + 32, 161);          // the brush model
  brush[4 + 34] = 100;                // view distance ratio
  brush[4 + 35] = 90;                 // level of detail ratio
  const float x = 229.9f;
  std::memcpy(brush.data() + 4 + 36 + 12, &x, 4);  // the matrix's first row's translation
  put32(brush, 4 + 88, 734);                       // the brush material
  std::vector<std::uint8_t> out;
  KCD2S_CHECK_EQ(kcd1::convert_object(brush.data(), brush.size(), 0, out), brush.size());
  KCD2S_CHECK_EQ(out.size(), std::size_t{4 + 100});
  std::uint16_t layer = 0;
  std::memcpy(&layer, out.data() + 4 + 24, 2);
  KCD2S_CHECK_EQ(int(layer), 3);
  std::uint64_t flags = 0;
  std::memcpy(&flags, out.data() + 4 + 28, 8);
  KCD2S_CHECK_EQ(flags, std::uint64_t{0x60000008u});
  std::uint16_t model = 0;
  std::memcpy(&model, out.data() + 4 + 36, 2);
  KCD2S_CHECK_EQ(int(model), 161);
  KCD2S_CHECK_EQ(int(out[4 + 38]), 100);
  KCD2S_CHECK_EQ(int(out[4 + 39]), 90);
  KCD2S_CHECK_EQ(float_at(out, 4 + 40 + 12), x);
  std::uint32_t material = 0;
  std::memcpy(&material, out.data() + 4 + 92, 4);
  KCD2S_CHECK_EQ(material, 734u);

  // A KCD1 plant: type 2, then 60 bytes, the same size in KCD2.
  std::vector<std::uint8_t> plant(4 + 60, 0);
  put32(plant, 0, 2);
  put16(plant, 4 + 32, 44);  // its vegetation group
  const float position[3] = {12.94f, 6.99f, 146.6f};
  std::memcpy(plant.data() + 4 + 36, position, 12);
  const float scale = 0.7344f;
  std::memcpy(plant.data() + 4 + 48, &scale, 4);
  plant[4 + 53] = 0x24;  // its angle
  KCD2S_CHECK_EQ(kcd1::convert_object(plant.data(), plant.size(), 0, out), plant.size());
  KCD2S_CHECK_EQ(out.size(), std::size_t{4 + 60});
  std::uint16_t group = 0;
  std::memcpy(&group, out.data() + 4 + 36, 2);
  KCD2S_CHECK_EQ(int(group), 44);
  KCD2S_CHECK_EQ(float_at(out, 4 + 40), position[0]);
  KCD2S_CHECK_EQ(float_at(out, 4 + 48), position[2]);
  KCD2S_CHECK_EQ(float_at(out, 4 + 52), scale);
  KCD2S_CHECK_EQ(int(out[4 + 57]), 0x24);

  // A merged mesh sector (type 23) of two entries is skipped whole, a kind not known is not.
  std::vector<std::uint8_t> grass(4 + 64 + 16, 0);
  put32(grass, 0, 23);
  put32(grass, 4 + 36, 2);
  KCD2S_CHECK_EQ(kcd1::convert_object(grass.data(), grass.size(), 0, out), grass.size());
  KCD2S_CHECK(out.empty());
  std::vector<std::uint8_t> unknown(64, 0);
  put32(unknown, 0, 99);
  KCD2S_CHECK_EQ(kcd1::convert_object(unknown.data(), unknown.size(), 0, out), std::size_t{0});
}

KCD2S_TEST(kcd1_level, a_terrain_with_a_version_5_octree_is_written_as_version_8) {
  // The smallest version 28 terrain: header, three brush models (a house and two of KCD1's
  // distant stand-ins, the map grid's and a town's), one 64 m leaf node of 65 x 65 heights, then
  // an octree root holding one plant, a brush of each model and no children.
  std::vector<std::uint8_t> terrain(32, 0);
  terrain[0] = 28;
  put32(terrain, 8, 64);                  // size, metres
  terrain.resize(terrain.size() + 4, 0);  // no plant kinds
  const std::size_t models = terrain.size();
  terrain.resize(models + 4 + 3 * 256, 0);
  put32(terrain, models, 3);
  const std::string house = "objects/buildings/house.cgf";
  const std::string stand_in = "Objects/uber/wh_modeluberlodnode_0_2.cgf";
  const std::string town_stand_in = "objects/uberlods/rataje/uberlod_rataje_city_a.cgf";
  std::memcpy(terrain.data() + models + 4, house.data(), house.size());
  std::memcpy(terrain.data() + models + 4 + 256, stand_in.data(), stand_in.size());
  std::memcpy(terrain.data() + models + 4 + 512, town_stand_in.data(), town_stand_in.size());
  terrain.resize(terrain.size() + 4, 0);  // no brush materials
  const std::size_t node = terrain.size();
  terrain.resize(node + 44, 0);
  put16(terrain, node, 7);
  put16(terrain, node + 36, 65);
  terrain.resize(terrain.size() + 65 * 65 * 2, 0);
  terrain.resize((terrain.size() + 3) & ~std::size_t(3), 0);
  terrain.resize(terrain.size() + 6 * 4, 0);
  const std::size_t octree = terrain.size();
  terrain.resize(octree + 32, 0);
  put16(terrain, octree, 5);
  put32(terrain, octree + 28, 4 + 60 + 3 * (4 + 96));
  const std::size_t plant = terrain.size();
  terrain.resize(plant + 4 + 60, 0);
  put32(terrain, plant, 2);
  for (std::uint16_t model = 0; model < 3; ++model) {
    const std::size_t brush = terrain.size();
    terrain.resize(brush + 4 + 96, 0);
    put32(terrain, brush, 1);
    put16(terrain, brush + 4 + 32, model);
  }
  put32(terrain, 4, std::uint32_t(terrain.size()));

  const auto layout = kcd1::terrain_layout(terrain);
  KCD2S_CHECK(layout.has_value());
  KCD2S_CHECK_EQ(layout->octree, octree);
  kcd1::ObjectCounts counts;
  const auto converted = kcd1::convert_terrain(terrain, counts);
  KCD2S_CHECK(converted.has_value());
  KCD2S_CHECK_EQ(counts.octree_nodes, std::size_t{1});
  KCD2S_CHECK_EQ(counts.kept[2], std::size_t{1});
  KCD2S_CHECK_EQ(counts.kept[1], std::size_t{1});
  KCD2S_CHECK_EQ(counts.distant_stand_ins, std::size_t{2});
  // The root's objects: the plant and the house's brush (64 + 104 bytes).
  std::int32_t objects = 0;
  std::memcpy(&objects, converted->data() + octree + 28, 4);
  KCD2S_CHECK_EQ(objects, 64 + 104);
  std::int16_t version = 0;
  std::memcpy(&version, converted->data() + octree, 2);
  KCD2S_CHECK_EQ(int(version), 8);
  std::int32_t total = 0;
  std::memcpy(&total, converted->data() + 4, 4);
  KCD2S_CHECK_EQ(std::size_t(total), converted->size());
  KCD2S_CHECK_EQ(kcd1::empty_indoor().size(), std::size_t{20});
}

KCD2S_TEST(kcd1_level, steam_libraries_and_the_install_folder_are_read_from_steams_files) {
  const std::string vdf =
      "\"libraryfolders\"\n{\n\t\"0\"\n\t{\n\t\t\"path\"\t\t\"C:\\\\Program Files "
      "(x86)\\\\Steam\"\n"
      "\t\t\"apps\"\n\t\t{\n\t\t\t\"379430\"\t\t\"1\"\n\t\t}\n\t}\n"
      "\t\"1\"\n\t{\n\t\t\"path\"\t\t\"D:\\\\SteamLibrary\"\n\t}\n}\n";
  const auto paths = kcd1::library_paths(vdf);
  KCD2S_CHECK_EQ(paths.size(), std::size_t{2});
  KCD2S_CHECK_EQ(paths[0].string(), std::string("C:\\Program Files (x86)\\Steam"));
  KCD2S_CHECK_EQ(paths[1].string(), std::string("D:\\SteamLibrary"));
  const std::string manifest =
      "\"AppState\"\n{"
      "\n\t\"appid\"\t\t\"379430\"\n\t\"installdir\"\t\t\"KingdomComeDeliverance\"\n}\n";
  KCD2S_CHECK_EQ(kcd1::install_folder(manifest), std::string("KingdomComeDeliverance"));
  KCD2S_CHECK(kcd1::install_folder("\"AppState\"\n{\n}\n").empty());
}

KCD2S_TEST(kcd1_level, a_pack_copies_entries_as_stored_and_reads_back) {
  const std::vector<std::pair<std::string, std::vector<std::uint8_t>>> source_entries = {
      {"terrain\\terrain.dat", {28, 0, 6, 1, 1, 2, 3, 4}},
      {"brush\\designer_1.cgf", {'C', 'r', 'C', 'h'}},
  };
  const std::vector<std::uint8_t> source = files::stored_zip_entries(source_entries);
  const auto source_read = files::zip_in_memory(source);
  const auto listed = files::zip_entries(source_read, source.size());
  KCD2S_CHECK(listed.has_value());

  const std::filesystem::path file = temporary("kcds-kcd1-pack-test.pak");
  {
    kcd1::PackWriter pack(file);
    KCD2S_CHECK(pack.add("levelinfo.xml", std::string("<LevelInfo />")));
    for (const auto& entry : *listed) {
      KCD2S_CHECK(pack.copy(source_read, entry));
    }
    KCD2S_CHECK(pack.finish());
  }
  std::uint64_t size = 0;
  const auto read = kcd1::pack_on_disk(file, size);
  const auto entries = files::zip_entries(read, size);
  KCD2S_CHECK(entries.has_value());
  KCD2S_CHECK_EQ(entries->size(), std::size_t{3});
  KCD2S_CHECK_EQ((*entries)[0].name, std::string("levelinfo.xml"));
  const auto info = files::zip_entry_bytes(read, (*entries)[0]);
  KCD2S_CHECK(info.has_value());
  KCD2S_CHECK_EQ(std::string(info->begin(), info->end()), std::string("<LevelInfo />"));
  for (std::size_t index = 0; index < source_entries.size(); ++index) {
    KCD2S_CHECK_EQ((*entries)[index + 1].name, source_entries[index].first);
    const auto bytes = files::zip_entry_bytes(read, (*entries)[index + 1]);
    KCD2S_CHECK(bytes.has_value());
    KCD2S_CHECK(*bytes == source_entries[index].second);
  }
  std::filesystem::remove(file);
}

KCD2S_TEST(kcd1_level, a_pack_of_more_than_65535_entries_reads_back_whole) {
  const std::filesystem::path file = temporary("kcds-kcd1-big-pack-test.pak");
  constexpr std::size_t kCount = 65537;
  {
    kcd1::PackWriter pack(file);
    for (std::size_t index = 0; index < kCount; ++index) {
      pack.add("terrain/merged_meshes_sectors/s" + std::to_string(index), std::string("x"));
    }
    KCD2S_CHECK(pack.finish());
  }
  std::uint64_t size = 0;
  const auto read = kcd1::pack_on_disk(file, size);
  const auto entries = files::zip_entries(read, size);
  KCD2S_CHECK(entries.has_value());
  KCD2S_CHECK_EQ(entries->size(), kCount);
  KCD2S_CHECK_EQ(entries->back().name, std::string("terrain/merged_meshes_sectors/s65536"));
  std::filesystem::remove(file);
}

KCD2S_TEST(kcd1_level, kcd1_entities_become_kcd2_doors_stashes_ladders_and_beds) {
  const std::string kcd1_objects =
      "<Objects>\n"
      "  <Entity Name=\"Door1\" Pos=\"1,2,3\" EntityClass=\"AnimDoor\" EntityId=\"10\" "
      "EntityGuid=\"42AD2F2C19992F00\">\n"
      "    <Properties fKeepOpenFrom=\"7.5\" fKeepOpenUntil=\"19.5\" "
      "object_Model=\"objects/characters/assets/doors/lvl1_door_02_left.cdf\" "
      "soclasses_SmartObjectClass=\"\" soclasses_SmartObjectHelpers=\"DoorRight\" "
      "guidSmartObjectType=\"4dbd19d3-64dc-445a-b3ce-05e1ce81d15a\">\n"
      "      <Lock bLocked=\"0\" esLockTypes=\"manual/key\" />\n"
      "    </Properties>\n"
      "  </Entity>\n"
      "  <Entity Name=\"Chest1\" Pos=\"4,5,6\" EntityClass=\"Stash\" EntityId=\"11\" "
      "EntityGuid=\"45C89E20BA462000\">\n"
      "    <Properties object_Model=\"objects/props/wooden_bins/chests/chest_02/chest02.cga\" "
      "soclasses_SmartObjectHelpers=\"Chest\" guidSmartObjectType=\"\">\n"
      "      <Database guidInventoryDBId=\"895bbe20-3599-4a89-b361-1c58ffc5d00b\" />\n"
      "    </Properties>\n"
      "  </Entity>\n"
      "  <Entity Name=\"Ladder1\" Pos=\"0,0,0\" EntityClass=\"Ladder\" EntityId=\"12\" "
      "EntityGuid=\"4920EA343B03E700\">\n"
      "    <Properties height=\"2.3\" soclasses_SmartObjectHelpers=\"Ladder_sm_a\" "
      "guidSmartObjectType=\"6ec5aa20-ecd7-47e2-913f-3f94d78e439e\" />\n"
      "  </Entity>\n"
      "  <Entity Name=\"SO_Bed\" Pos=\"10,20,30\" Rotate=\"1,0,0,0\" EntityClass=\"Bed\" "
      "EntityId=\"13\" EntityGuid=\"4174282E36CC0F00\">\n"
      "    <Properties object_Model=\"objects/buildings/refugee_camp/bad_straw.cgf\" "
      "soclasses_SmartObjectHelpers=\"Bed\" />\n"
      "  </Entity>\n"
      "  <Entity Name=\"Npc1\" Pos=\"0,0,0\" EntityClass=\"NPC\" EntityId=\"14\" />\n"
      "  <Object Type=\"Decal\" Name=\"Decal1\" Pos=\"0,0,0\" />\n"
      "</Objects>\n";
  kcd1::EntityCounts counts;
  const std::string out = kcd1::translate_objects(kcd1_objects, counts);
  KCD2S_CHECK_EQ(counts.kept["AnimDoor"], std::size_t{1});
  KCD2S_CHECK_EQ(counts.kept["Stash"], std::size_t{1});
  KCD2S_CHECK_EQ(counts.kept["Ladder"], std::size_t{1});
  KCD2S_CHECK_EQ(counts.kept["Bed"], std::size_t{1});
  KCD2S_CHECK_EQ(counts.left_out, std::size_t{2});
  KCD2S_CHECK(has(out, "<Objects>\n"));
  KCD2S_CHECK(has(out, "</Objects>\n"));
  // The door: KCD2's animation set, helper and type; KCD1's open hours on its lock.
  KCD2S_CHECK(has(out, "EntityGuid=\"42ad2f2c-1999-2f00\""));
  KCD2S_CHECK(has(out, "esDoorAnimSet=\"VillageRight\""));
  KCD2S_CHECK(has(out, "soclass_SmartObjectHelpers=\"DoorRight\""));
  KCD2S_CHECK(has(out, "guidSmartObjectType=\"49515779-1a9d-330c-4768-c9326ea1ca96\""));
  KCD2S_CHECK(has(out, "fKeepUnlockedFrom=\"7.5\""));
  KCD2S_CHECK(has(out, "fKeepUnlockedTo=\"19.5\""));
  KCD2S_CHECK(!has(out, "soclasses_"));
  // The chest: one of KCD2's generated inventories instead of KCD1's database id.
  KCD2S_CHECK(has(out, "sGeneratedInventory=\"inventory_household\""));
  KCD2S_CHECK(!has(out, "guidInventoryDBId"));
  // The ladder: KCD2's helper of the nearest height.
  KCD2S_CHECK(has(out, "soclass_SmartObjectHelpers=\"Ladder_225\""));
  // The bed: its model, the place to lie on and the trigger, linked both ways.
  KCD2S_CHECK(has(out, "EntityClass=\"GeomEntity\" EntityId=\"13\""));
  KCD2S_CHECK(has(out, "Geometry=\"objects/buildings/refugee_camp/bad_straw.cgf\""));
  KCD2S_CHECK(has(out, "EntityClass=\"StanceSmartObject\" EntityId=\"100000\""));
  KCD2S_CHECK(has(out, "soclass_SmartObjectHelpers=\"Bed_1Place_High\""));
  KCD2S_CHECK(has(out,
                  "<Link TargetId=\"100001\" TargetGuid=\"00000000-0000-0000\" "
                  "Name=\"mTrigger\" />"));
  KCD2S_CHECK(has(out, "EntityClass=\"BedTrigger\" EntityId=\"100001\""));
  KCD2S_CHECK(has(out, "Pos=\"10,20,30.4\""));
  KCD2S_CHECK(has(out, "<Link TargetId=\"100000\" TargetGuid=\"b174282e-36cc-0f00\""));
  // KCD1's people and decals stay out.
  KCD2S_CHECK(!has(out, "Npc1"));
  KCD2S_CHECK(!has(out, "Decal1"));
}
