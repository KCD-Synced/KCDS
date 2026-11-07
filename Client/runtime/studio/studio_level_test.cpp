#include "studio_level.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "../files/stored_zip.hpp"
#include "../files/zip_read.hpp"
#include "kcd2s/testkit.hpp"

namespace files = kcds::runtime::files;
namespace level = kcds::runtime::studio::level;

namespace {

using Bytes = std::vector<std::uint8_t>;

Bytes bytes_of(const std::string& text) {
  return Bytes(text.begin(), text.end());
}

std::string text_of(const Bytes& bytes) {
  return std::string(bytes.begin(), bytes.end());
}

// A small klaster level.pak with the files the studio's level is made of, named as the game's
// packs name them (some with the other slash), and a file it leaves out.
std::vector<std::pair<std::string, Bytes>> klaster_files() {
  return {
      {"levelinfo.xml", bytes_of("<LevelInfo Name=\"data/levels/klaster\" />")},
      {"leveldata.xml",
       bytes_of("<LevelData Name=\"klaster\"><Layers><Layer Name=\"town\" /></Layers>"
                "<SurfaceTypes /><Vegetation><Object /></Vegetation></LevelData>")},
      {"leveldataaction.xml", bytes_of("<Action CGFCount=\"3789\" ProgressBarRange=\"3789\" />")},
      {"mission_mission0.xml", bytes_of("<Mission CGFCount=\"12\"><Env /></Mission>")},
      {"levelprototypes.xml", bytes_of("<Prototypes />")},
      {"serialize.xml", bytes_of("<Serialize />")},
      {"lensflarelist.xml", bytes_of("<LensFlares />")},
      {"dyntexsrclayeract.xml", bytes_of("<A />")},
      {"shaderslist.txt", Bytes()},
      {"Tables\\rpg\\soul.xml", bytes_of("<soul />")},
      {"tables/item.xml", bytes_of("<item />")},
      {"terrain/terrain.dat", bytes_of("klaster's own ground")},
  };
}

// The pak's files by name and their order; false when an entry does not read.
bool read_pak(const Bytes& pak, std::map<std::string, Bytes>& out,
              std::vector<std::string>& order) {
  const auto read = files::zip_in_memory(pak);
  const auto entries = files::zip_entries(read, pak.size());
  if (!entries) {
    return false;
  }
  for (const files::ZipEntry& entry : *entries) {
    order.push_back(entry.name);
    const auto bytes = files::zip_entry_bytes(read, entry);
    if (!bytes) {
      return false;
    }
    out[entry.name] = *bytes;
  }
  return true;
}

}  // namespace

KCD2S_TEST(studio_level, the_ground_is_the_studio_tools_byte_for_byte) {
  // Dev/tools/assets/kcds_studio/make_studio_level.py: studio_terrain() and
  // studio_ground_texture() are 68036 and 327764 bytes with these CRCs.
  const Bytes terrain = level::terrain();
  KCD2S_CHECK_EQ(terrain.size(), std::size_t(68036));
  KCD2S_CHECK_EQ(files::zip_crc32(terrain.data(), terrain.size()), 0x2eabfe5cu);
  const Bytes texture = level::ground_texture();
  KCD2S_CHECK_EQ(texture.size(), std::size_t(327764));
  KCD2S_CHECK_EQ(files::zip_crc32(texture.data(), texture.size()), 0x7ecde7c4u);
}

KCD2S_TEST(studio_level, klaster_is_emptied_renamed_and_given_the_studio_floor) {
  const Bytes klaster = files::stored_zip_entries(klaster_files());
  const auto pak = level::build(files::zip_in_memory(klaster), klaster.size());
  KCD2S_CHECK(pak.has_value());
  if (!pak) {
    return;
  }
  std::vector<std::string> order;
  std::map<std::string, Bytes> level_files;
  KCD2S_CHECK(read_pak(*pak, level_files, order));
  const std::vector<std::string> expected = {
      "levelinfo.xml",         "leveldata.xml",       "leveldataaction.xml", "mission_mission0.xml",
      "objects_mission0.xml",  "levelprototypes.xml", "serialize.xml",       "lensflarelist.xml",
      "dyntexsrclayeract.xml", "shaderslist.txt",     "brushlist.txt",       "Tables/rpg/soul.xml",
      "tables/item.xml",       "terrain/cover.ctc",   "terrain/terrain.dat", "terrain/indoor.dat",
      "terrain/hlods.xml",     "terrain/hlods.dat"};
  KCD2S_CHECK(order == expected);
  KCD2S_CHECK_EQ(text_of(level_files.at("levelinfo.xml")),
                 std::string("<LevelInfo Name=\"data/levels/kcds_studio\" />"));
  KCD2S_CHECK_EQ(text_of(level_files.at("leveldata.xml")),
                 std::string("<LevelData Name=\"kcds_studio\"><Layers /><SurfaceTypes />"
                             "<Vegetation /></LevelData>"));
  KCD2S_CHECK_EQ(text_of(level_files.at("leveldataaction.xml")),
                 std::string("<Action CGFCount=\"0\" ProgressBarRange=\"1\" />"));
  KCD2S_CHECK_EQ(text_of(level_files.at("mission_mission0.xml")),
                 std::string("<Mission CGFCount=\"0\"><Env /></Mission>"));
  KCD2S_CHECK_EQ(text_of(level_files.at("objects_mission0.xml")),
                 std::string("<Objects>\n</Objects>\n"));
  KCD2S_CHECK_EQ(text_of(level_files.at("Tables/rpg/soul.xml")), std::string("<soul />"));
  KCD2S_CHECK(level_files.at("terrain/terrain.dat") == level::terrain());
  KCD2S_CHECK_EQ(level_files.at("terrain/indoor.dat").size(), std::size_t(20));
}

KCD2S_TEST(studio_level, a_missing_level_file_makes_no_level) {
  auto without_mission = klaster_files();
  without_mission.erase(without_mission.begin() + 3);
  const Bytes klaster = files::stored_zip_entries(without_mission);
  KCD2S_CHECK(!level::build(files::zip_in_memory(klaster), klaster.size()).has_value());
}
