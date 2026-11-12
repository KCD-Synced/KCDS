#include "level_pack.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "../files/zip_read.hpp"
#include "entities.hpp"
#include "install.hpp"
#include "level_files.hpp"
#include "pack_writer.hpp"
#include "terrain_convert.hpp"

namespace kcds::runtime::kcd1 {
namespace {

// Raised when what goes into the packs changes, so older builds are made again.
constexpr int kBuildVersion = 11;
constexpr const char* kStampName = "build.txt";
// KCD1's packs of models and buildings, opened where they are (kcd1_map.cpp).
constexpr const wchar_t* kModelPacks[] = {L"Objects.pak", L"Buildings.pak"};

// The level description files taken from KCD2's level as they are.
// smartobjecthelpersetanimations.xml: the animations of KCD2's smart object helper sets (beds,
// doors, ladders, seats...), the same in every level; without it the AI never starts a use of one
// (a player asked to lie in a bed stays standing).
constexpr const char* kDescriptionFiles[] = {
    "levelprototypes.xml",   "serialize.xml",   "lensflarelist.xml",
    "dyntexsrclayeract.xml", "shaderslist.txt", "smartobjecthelpersetanimations.xml"};

std::filesystem::path output_folder() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(local) / L"KCDS" / L"kcd1";
}

std::string normalised(const std::string& name) {
  std::string out = name;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](char c) { return c == '\\' ? '/' : char(std::tolower(unsigned(c))); });
  return out;
}

// A source's size and write time, as the stamp keeps it.
std::string stamp_line(const std::filesystem::path& file) {
  std::error_code error;
  const auto size = std::filesystem::file_size(file, error);
  const auto time = std::filesystem::last_write_time(file, error);
  return utf8(file) + " " + std::to_string(error ? 0 : size) + " " +
         std::to_string(time.time_since_epoch().count()) + "\n";
}

std::string read_text(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// An open pack: its reader and entries by normalised name.
struct Source {
  files::ZipReadAt read;
  std::vector<files::ZipEntry> entries;
  std::map<std::string, std::size_t> by_name;

  bool open(const std::filesystem::path& file) {
    std::uint64_t size = 0;
    read = pack_on_disk(file, size);
    if (!read) {
      return false;
    }
    auto listed = files::zip_entries(read, size);
    if (!listed) {
      return false;
    }
    entries = std::move(*listed);
    for (std::size_t index = 0; index < entries.size(); ++index) {
      by_name[normalised(entries[index].name)] = index;
    }
    return true;
  }

  std::string text(const std::string& name) const {
    const auto found = by_name.find(name);
    if (found == by_name.end()) {
      return {};
    }
    const auto bytes = files::zip_entry_bytes(read, entries[found->second]);
    return bytes ? std::string(bytes->begin(), bytes->end()) : std::string();
  }
};

// The level pack: KCD2's description of `source` (a KCD2 level), KCD1's world.
bool write_level(const Source& kcd1, const Source& kcd2, const std::filesystem::path& file,
                 Logger log) {
  PackWriter pack(file);
  pack.add("levelinfo.xml", level_info(kcd2.text("levelinfo.xml"), kcd1.text("levelinfo.xml")));
  pack.add("leveldata.xml", level_data(kcd2.text("leveldata.xml"), kcd1.text("leveldata.xml")));
  pack.add("leveldataaction.xml", level_action(kcd2.text("leveldataaction.xml")));
  pack.add("mission_mission0.xml", mission(kcd2.text("mission_mission0.xml")));
  // KCD1's doors, chests, beds... on KCD2's entity classes (entities.hpp).
  EntityCounts entities;
  pack.add("objects_mission0.xml", translate_objects(kcd1.text("objects_mission0.xml"), entities));
  if (log) {
    for (const auto& [kind, count] : entities.kept) {
      log("KCD1 entities %s: %zu", kind.c_str(), count);
    }
    log("KCD1 entities left out: %zu", entities.left_out);
  }
  pack.add("brushlist.txt", std::string());
  for (const char* name : kDescriptionFiles) {
    pack.add(name, kcd2.text(name));
  }
  for (const files::ZipEntry& entry : kcd2.entries) {
    if (normalised(entry.name).rfind("tables/", 0) == 0) {
      pack.copy(kcd2.read, entry);
    }
  }
  // No hierarchical LODs: one empty root, as a custom map has.
  pack.add("terrain/hlods.xml",
           std::string("<HLod DataOffset=\"4\" DataSize=\"4\" ProxyIndex=\"-1\" Type=\"Cluster\" "
                       "Radius=\"8000\" NearestObserverDistance=\"0\" Name=\"root\" "
                       "Hash=\"214082742\" />\n"));
  pack.add("terrain/hlods.dat", std::vector<std::uint8_t>{2, 0, 0, 0, 0, 0, 0, 0});
  std::size_t copied = 0;
  bool terrain_converted = false;
  for (const files::ZipEntry& entry : kcd1.entries) {
    const std::string name = normalised(entry.name);
    if (!level_file(entry.name)) {
      continue;
    }
    if (name == "terrain/terrain.dat") {
      // The terrain with its objects as KCD2 reads them (terrain_convert.hpp).
      const auto bytes = files::zip_entry_bytes(kcd1.read, entry);
      ObjectCounts counts;
      const auto converted = bytes ? convert_terrain(*bytes, counts) : std::nullopt;
      if (!converted) {
        if (log) {
          log("KCD1 terrain.dat could NOT be converted (%zu octree nodes read)",
              counts.octree_nodes);
        }
        return false;
      }
      pack.add("terrain/terrain.dat", *converted);
      terrain_converted = true;
      if (log) {
        for (const auto& [type, count] : counts.kept) {
          log("KCD1 terrain objects of type %d: %zu kept", type, count);
        }
        for (const auto& [type, count] : counts.dropped) {
          log("KCD1 terrain objects of type %d: %zu left out", type, count);
        }
        log("KCD1 terrain: %zu distant stand-ins for buildings left out", counts.distant_stand_ins);
      }
    } else if (name == "terrain/indoor.dat") {
      pack.add("terrain/indoor.dat", empty_indoor());
    } else {
      pack.copy(kcd1.read, entry);
    }
    copied += 1;
  }
  const bool written = terrain_converted && pack.finish();
  if (log) {
    log("KCD1 level pack %s: %zu entries, %zu from KCD1", written ? "written" : "NOT written",
        pack.entries(), copied);
  }
  return written;
}

// A texture header of `source` with its persistent mip count set (level_files.hpp
// texture_header); nullopt when it needs no change or cannot be read.
std::optional<std::vector<std::uint8_t>> fixed_header(const Source& source,
                                                      const files::ZipEntry& entry) {
  const std::string name = normalised(entry.name);
  const bool attached_alpha = name.size() > 2 && name.compare(name.size() - 2, 2, ".a") == 0;
  int split_files = 0;
  while (split_files < 32 && source.by_name.count(split_file(name, split_files + 1)) != 0) {
    split_files += 1;
  }
  auto bytes = files::zip_entry_bytes(source.read, entry);
  if (!bytes || !fix_persistent_mips(*bytes, attached_alpha, split_files)) {
    return std::nullopt;
  }
  return bytes;
}

bool is_material(const std::string& name) {
  const std::string path = normalised(name);
  return path.size() > 4 && path.compare(path.size() - 4, 4, ".mtl") == 0;
}

// A material of `source` as KCD2's shaders read it (level_files.hpp translate_material); nullopt
// when it needs no change or cannot be read.
std::optional<std::string> translated_material(const Source& source, const files::ZipEntry& entry) {
  const auto bytes = files::zip_entry_bytes(source.read, entry);
  if (!bytes) {
    return std::nullopt;
  }
  const std::string text(bytes->begin(), bytes->end());
  std::string translated = translate_material(text);
  if (translated == text) {
    return std::nullopt;
  }
  return translated;
}

// What the assets pack changed for KCD2.
struct AssetCounts {
  std::size_t headers = 0;
  std::size_t materials = 0;
};

// One file of `source` for the assets pack: a texture header or material set for KCD2 when it
// needs it, else (`copy_as_is`) the file as KCD1 stores it.
void add_asset(PackWriter& pack, const Source& source, const files::ZipEntry& entry,
               bool copy_as_is, AssetCounts& counts) {
  if (texture_header(entry.name)) {
    const auto fixed = fixed_header(source, entry);
    if (fixed) {
      pack.add(entry.name, *fixed);
      counts.headers += 1;
      return;
    }
  } else if (is_material(entry.name)) {
    const auto translated = translated_material(source, entry);
    if (translated) {
      pack.add(entry.name, *translated);
      counts.materials += 1;
      return;
    }
  }
  if (copy_as_is) {
    pack.copy(source.read, entry);
  }
}

// The files of `source` that `wanted` takes.
template <class Wanted>
void add_filtered(PackWriter& pack, const Source& source, Wanted wanted, AssetCounts& counts) {
  for (const files::ZipEntry& entry : source.entries) {
    if (wanted(entry.name)) {
      add_asset(pack, source, entry, true, counts);
    }
  }
}

// KCD1's materials, its world's textures, its animated props and its models' and buildings'
// texture headers and materials, set for KCD2 (this pack is found before KCD1's Objects and
// Buildings packs).
bool write_assets(const Source& game_data, const std::vector<Source>& textures,
                  const Source& characters, const std::vector<Source>& models,
                  const std::filesystem::path& file, Logger log) {
  PackWriter pack(file);
  AssetCounts counts;
  add_filtered(
      pack, game_data, [](const std::string& name) { return material_file(name); }, counts);
  const std::size_t materials = pack.entries();
  for (const Source& source : textures) {
    add_filtered(pack, source, [](const std::string& name) { return texture_file(name); }, counts);
  }
  const std::size_t textures_added = pack.entries() - materials;
  add_filtered(pack, characters, [](const std::string& name) { return prop_file(name); }, counts);
  const std::size_t props = pack.entries() - materials - textures_added;
  for (const Source& source : models) {
    for (const files::ZipEntry& entry : source.entries) {
      add_asset(pack, source, entry, false, counts);
    }
  }
  const bool written = pack.finish();
  if (log) {
    log("KCD1 assets pack %s: %zu materials, %zu textures, %zu props; %zu texture headers and %zu "
        "materials set for KCD2",
        written ? "written" : "NOT written", materials, textures_added, props, counts.headers,
        counts.materials);
  }
  return written;
}

// Writes `file` through a temporary one, so a pack is never left half written.
template <class Write>
bool write_through(const std::filesystem::path& file, Write write) {
  std::filesystem::path part = file;
  part += L".part";
  if (!write(part)) {
    std::error_code ignored;
    std::filesystem::remove(part, ignored);
    return false;
  }
  std::error_code error;
  std::filesystem::rename(part, file, error);
  return !error;
}

}  // namespace

std::optional<Packs> build_packs(const std::filesystem::path& kcd1,
                                 const std::filesystem::path& kcd2, Logger log) {
  const std::filesystem::path folder = output_folder();
  if (folder.empty()) {
    return std::nullopt;
  }
  const std::filesystem::path kcd1_level = kcd1 / L"Data" / L"Levels" / L"rataje" / L"level.pak";
  const std::filesystem::path game_data = kcd1 / L"Data" / L"GameData.pak";
  const std::filesystem::path characters = kcd1 / L"Data" / L"Characters.pak";
  // KCD2's description: klaster's (a DLC), else Trosecko's.
  std::filesystem::path kcd2_level = kcd2 / L"Data" / L"Levels" / L"klaster" / L"level.pak";
  std::error_code error;
  if (!std::filesystem::is_regular_file(kcd2_level, error)) {
    kcd2_level = kcd2 / L"Data" / L"Levels" / L"trosecko" / L"level.pak";
  }
  // KCD1's texture packs (the DLC's when it is there), and its model packs, whose texture
  // headers are set.
  std::vector<std::filesystem::path> texture_packs;
  for (const wchar_t* name : {L"Textures.pak", L"Textures_DLC4.pak"}) {
    if (std::filesystem::is_regular_file(kcd1 / L"Data" / name, error)) {
      texture_packs.push_back(kcd1 / L"Data" / name);
    }
  }
  std::vector<std::filesystem::path> model_packs;
  for (const wchar_t* name : kModelPacks) {
    model_packs.push_back(kcd1 / L"Data" / name);
  }
  Packs packs;
  packs.level = folder / (std::string(kLevel) + ".pak");
  packs.assets = folder / L"assets.pak";
  std::string stamp = "kcd1 packs " + std::to_string(kBuildVersion) + "\n" +
                      stamp_line(kcd1_level) + stamp_line(game_data) + stamp_line(kcd2_level) +
                      stamp_line(characters);
  for (const auto& texture_pack : texture_packs) {
    stamp += stamp_line(texture_pack);
  }
  for (const auto& model_pack : model_packs) {
    stamp += stamp_line(model_pack);
  }
  if (read_text(folder / kStampName) == stamp &&
      std::filesystem::is_regular_file(packs.level, error) &&
      std::filesystem::is_regular_file(packs.assets, error)) {
    return packs;
  }
  std::filesystem::create_directories(folder, error);
  const auto started = std::chrono::steady_clock::now();
  Source kcd1_source;
  Source kcd2_source;
  Source game_data_source;
  Source characters_source;
  std::vector<Source> texture_sources(texture_packs.size());
  std::vector<Source> model_sources(model_packs.size());
  bool opened = kcd1_source.open(kcd1_level) && kcd2_source.open(kcd2_level) &&
                game_data_source.open(game_data) && characters_source.open(characters);
  for (std::size_t index = 0; index < texture_packs.size(); ++index) {
    opened = opened && texture_sources[index].open(texture_packs[index]);
  }
  for (std::size_t index = 0; index < model_packs.size(); ++index) {
    opened = opened && model_sources[index].open(model_packs[index]);
  }
  if (!opened) {
    if (log) {
      log("KCD1 packs: a source pack could not be read (%s, %s, %s or the textures)",
          utf8(kcd1_level).c_str(), utf8(kcd2_level).c_str(), utf8(game_data).c_str());
    }
    return std::nullopt;
  }
  std::filesystem::remove(folder / kStampName, error);
  // A pack of an older build that is no longer used.
  std::filesystem::remove(folder / L"materials.pak", error);
  const bool level_written = write_through(packs.level, [&](const std::filesystem::path& file) {
    return write_level(kcd1_source, kcd2_source, file, log);
  });
  const bool assets_written =
      level_written && write_through(packs.assets, [&](const std::filesystem::path& file) {
        return write_assets(game_data_source, texture_sources, characters_source, model_sources,
                            file, log);
      });
  if (!assets_written) {
    return std::nullopt;
  }
  std::ofstream(folder / kStampName, std::ios::binary | std::ios::trunc) << stamp;
  if (log) {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    log("KCD1 packs built in %.1f s into %s", seconds.count(), utf8(folder).c_str());
  }
  return packs;
}

}  // namespace kcds::runtime::kcd1
