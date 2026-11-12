// The KCD1 map's level files and packs: what is taken from KCD1, what from KCD2, and packs that
// read back entry for entry.
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "../../files/stored_zip.hpp"
#include "../../files/zip_read.hpp"
#include "../install.hpp"
#include "../level_files.hpp"
#include "../pack_writer.hpp"
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
