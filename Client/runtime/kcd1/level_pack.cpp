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
#include "level_files.hpp"
#include "pack_writer.hpp"

namespace kcds::runtime::kcd1 {
namespace {

// Raised when what goes into the packs changes, so older builds are made again.
constexpr int kBuildVersion = 1;
constexpr const char* kStampName = "build.txt";

// The level description files taken from KCD2's level as they are.
constexpr const char* kDescriptionFiles[] = {"levelprototypes.xml", "serialize.xml",
                                             "lensflarelist.xml", "dyntexsrclayeract.xml",
                                             "shaderslist.txt"};

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
  return file.u8string() + " " + std::to_string(error ? 0 : size) + " " +
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
  pack.add("objects_mission0.xml", std::string("<Objects>\n</Objects>\n"));
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
  for (const files::ZipEntry& entry : kcd1.entries) {
    if (level_file(entry.name)) {
      pack.copy(kcd1.read, entry);
      copied += 1;
    }
  }
  const bool written = pack.finish();
  if (log) {
    log("KCD1 level pack %s: %zu entries, %zu from KCD1", written ? "written" : "NOT written",
        pack.entries(), copied);
  }
  return written;
}

bool write_materials(const Source& game_data, const std::filesystem::path& file, Logger log) {
  PackWriter pack(file);
  for (const files::ZipEntry& entry : game_data.entries) {
    if (material_file(entry.name)) {
      pack.copy(game_data.read, entry);
    }
  }
  const bool written = pack.finish();
  if (log) {
    log("KCD1 materials pack %s: %zu entries", written ? "written" : "NOT written", pack.entries());
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
  // KCD2's description: klaster's (a DLC), else Trosecko's.
  std::filesystem::path kcd2_level = kcd2 / L"Data" / L"Levels" / L"klaster" / L"level.pak";
  std::error_code error;
  if (!std::filesystem::is_regular_file(kcd2_level, error)) {
    kcd2_level = kcd2 / L"Data" / L"Levels" / L"trosecko" / L"level.pak";
  }
  Packs packs;
  packs.level = folder / (std::string(kLevel) + ".pak");
  packs.materials = folder / L"materials.pak";
  const std::string stamp = "kcd1 packs " + std::to_string(kBuildVersion) + "\n" +
                            stamp_line(kcd1_level) + stamp_line(game_data) + stamp_line(kcd2_level);
  if (read_text(folder / kStampName) == stamp &&
      std::filesystem::is_regular_file(packs.level, error) &&
      std::filesystem::is_regular_file(packs.materials, error)) {
    return packs;
  }
  std::filesystem::create_directories(folder, error);
  const auto started = std::chrono::steady_clock::now();
  Source kcd1_source;
  Source kcd2_source;
  Source game_data_source;
  if (!kcd1_source.open(kcd1_level) || !kcd2_source.open(kcd2_level) ||
      !game_data_source.open(game_data)) {
    if (log) {
      log("KCD1 packs: a source pack could not be read (%s, %s, %s)", kcd1_level.u8string().c_str(),
          kcd2_level.u8string().c_str(), game_data.u8string().c_str());
    }
    return std::nullopt;
  }
  std::filesystem::remove(folder / kStampName, error);
  const bool level_written = write_through(packs.level, [&](const std::filesystem::path& file) {
    return write_level(kcd1_source, kcd2_source, file, log);
  });
  const bool materials_written =
      level_written && write_through(packs.materials, [&](const std::filesystem::path& file) {
        return write_materials(game_data_source, file, log);
      });
  if (!materials_written) {
    return std::nullopt;
  }
  std::ofstream(folder / kStampName, std::ios::binary | std::ios::trunc) << stamp;
  if (log) {
    const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started);
    log("KCD1 packs built in %.1f s into %s", seconds.count(), folder.u8string().c_str());
  }
  return packs;
}

}  // namespace kcds::runtime::kcd1
