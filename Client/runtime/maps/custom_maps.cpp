#include "custom_maps.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "../files/base64.hpp"
#include "../files/stored_zip.hpp"
#include "../session/resource_cache.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::maps::custom {
namespace {

// ICryPak, by byte offset in its vtable (WHGame 1.5.5).
constexpr std::size_t kAdjustFileName = 8;
constexpr std::size_t kOpenPackBound = 48;
constexpr std::size_t kOpenPackNamed = 56;
constexpr std::size_t kOpenPacksBound = 0x48;
// As the engine opens a mod's paks.
constexpr unsigned kModPakFlags = 0x10400;
constexpr const char* kPrefix = "kcds_map_";
// Where the game looks for a custom map's navmesh (its level folder path is empty there).
constexpr const char* kUserFolder = "%USER%/";
constexpr std::size_t kLongestName = 32;
constexpr unsigned kSetWorldPosReason = 0x400000u;
// CCryAction's level system (GetILevelSystem), and CLevelSystem::Rescan (vtable slot 10,
// empties the level list, scans "levels" again and finds the current level again by
// its name), with the folder and tag CLevelSystem's constructor scans with.
constexpr std::size_t kLevelSystemAt = 1296;
constexpr std::size_t kRescanSlot = 10;
constexpr std::uintptr_t kRescanRva = 0x17DF5F8;
constexpr unsigned kMainLevelsTag = 0x4D41494Eu;
// The most a map's files from a server may weigh, bytes.
constexpr std::size_t kMostPack = 32u * 1024u * 1024u;
constexpr std::size_t kMostAbout = 4096;

using AdjustFileName = const char*(__fastcall*)(void* pak, const char* path, char* out,
                                                unsigned flags);
using OpenPackBound = bool(__fastcall*)(void* pak, const char* binding_root, const char* name,
                                        unsigned flags, void* memory, void* full_path);
using OpenPackNamed = bool(__fastcall*)(void* pak, const char* name, unsigned flags, void* memory,
                                        void* full_path);
using OpenPacksBound = bool(__fastcall*)(void* pak, const char* binding_root, const char* wildcard,
                                         unsigned flags, void* full_paths);
using WorldMatrix = const float*(__fastcall*)(void* entity);
using SetWorldMatrix = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);
using Rescan = void(__fastcall*)(void* level_system, const char* folder, unsigned tag);

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

Logger log_line = nullptr;
OpenPackNamed original_open_pack = nullptr;
std::mutex maps_mutex;
// The custom maps the game lists, by level name (lower case).
std::set<std::string> found;
// Maps a server's resource brought, written and waiting to be listed.
std::set<std::string> arrived;
// A map's file coming in pieces, by "<level>/<name>".
struct Incoming {
  std::vector<std::uint8_t> bytes;
  std::size_t received = 0;
};
std::map<std::string, Incoming> incoming;
// CryPak and its binding root for the mods' paks, kept from mount (engine thread).
void* crypak = nullptr;
std::string pak_root;
// The marker packs written by this game.
unsigned markers = 0;

std::filesystem::path maps_folder() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(local) / L"KCDS" / L"maps";
}

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](char c) { return c == '\\' ? '/' : char(std::tolower(unsigned(c))); });
  return text;
}

// The map's pack as a narrow path CryPak opens (plain ASCII only), empty for none.
std::string pack_path(const std::string& level) {
  const std::filesystem::path file = maps_folder() / level / L"level.pak";
  std::error_code error;
  if (!std::filesystem::is_regular_file(file, error)) {
    return {};
  }
  const std::wstring wide = file.wstring();
  std::string narrow;
  for (const wchar_t c : wide) {
    if (c > 0x7F) {
      return {};
    }
    narrow += char(c);
  }
  return narrow;
}

// Another pack of the map's folder (recast.pak) as a narrow path, empty when it is not there.
std::string side_pack(const std::string& level, const wchar_t* name) {
  const std::filesystem::path file = maps_folder() / level / name;
  std::error_code error;
  if (!std::filesystem::is_regular_file(file, error)) {
    return {};
  }
  std::string narrow;
  for (const wchar_t c : file.wstring()) {
    if (c > 0x7F) {
      return {};
    }
    narrow += char(c);
  }
  return narrow;
}

// A navmesh baked last time (recast.next.pak) takes the place of the map's recast.pak, before
// the game has either open.
void take_next_navmesh(const std::string& level) {
  const std::filesystem::path folder = maps_folder() / level;
  std::error_code error;
  if (!std::filesystem::is_regular_file(folder / L"recast.next.pak", error)) {
    return;
  }
  std::filesystem::remove(folder / L"recast.pak", error);
  std::filesystem::rename(folder / L"recast.next.pak", folder / L"recast.pak", error);
  if (log_line) {
    log_line("MAPS %s: the navmesh baked last time %s", level.c_str(),
             error ? "could NOT be taken" : "taken");
  }
}

// The custom map a level pack's path names ("levels/kcds_map_x/level.pak"), empty for another.
std::string level_of(const char* name) {
  if (!name) {
    return {};
  }
  const std::string path = lower(name);
  const std::size_t at = path.find(std::string("levels/") + kPrefix);
  const std::string tail = "/level.pak";
  if (at == std::string::npos || path.size() < tail.size() ||
      path.compare(path.size() - tail.size(), tail.size(), tail) != 0) {
    return {};
  }
  const std::size_t start = at + 7;
  const std::size_t end = path.find('/', start);
  return end == std::string::npos ? std::string() : path.substr(start, end - start);
}

// The level pack's opening: a custom map's real pack, bound at the level's folder; any other
// as the game opens it.
bool __fastcall hooked_open_pack(void* pak, const char* name, unsigned flags, void* memory,
                                 void* full_path) {
  const std::string level = level_of(name);
  bool known = false;
  {
    std::lock_guard<std::mutex> lock(maps_mutex);
    known = !level.empty() && found.count(level) != 0;
  }
  const std::string real = known ? pack_path(level) : std::string();
  if (real.empty()) {
    return original_open_pack(pak, name, flags, memory, full_path);
  }
  // The folder as a lookup adjusts it (the game's data folder in front), the pack's root.
  const std::string folder = std::string("levels/") + level + "/";
  char adjusted[2048] = {};
  const char* root = slot<AdjustFileName>(pak, kAdjustFileName)(pak, folder.c_str(), adjusted, 0);
  std::string binding = root ? std::string(root) : std::string();
  if (!binding.empty() && binding.back() != '\\' && binding.back() != '/') {
    binding += '\\';
  }
  const bool opened =
      !binding.empty() && slot<OpenPackBound>(pak, kOpenPackBound)(
                              pak, binding.c_str(), real.c_str(), flags, memory, full_path);
  if (log_line) {
    log_line("MAPS %s: level pack %s from %s at %s", level.c_str(),
             opened ? "opened" : "NOT opened", real.c_str(), binding.c_str());
  }
  // The map's navmesh pack beside it (Recast/RDNavmesh_1.hdr and .nav). The game's navigation reads
  // a level's navmesh from the level's folder, but for a custom map that path is empty and it
  // reads %USER%/Recast/RDNavmesh_1.hdr instead (NAVPROBE): the pack is
  // bound at the %USER% folder, so those files are the map's.
  const std::string navigation = side_pack(level, L"recast.pak");
  if (opened && !navigation.empty()) {
    char user[2048] = {};
    const char* user_root = slot<AdjustFileName>(pak, kAdjustFileName)(pak, kUserFolder, user, 0);
    std::string user_binding = user_root ? std::string(user_root) : std::string();
    if (!user_binding.empty() && user_binding.back() != '\\' && user_binding.back() != '/') {
      user_binding += '\\';
    }
    const bool navigation_opened =
        !user_binding.empty() &&
        slot<OpenPackBound>(pak, kOpenPackBound)(pak, user_binding.c_str(), navigation.c_str(),
                                                 flags, nullptr, nullptr);
    if (log_line) {
      log_line("MAPS %s: navmesh pack %s at %s", level.c_str(),
               navigation_opened ? "opened" : "NOT opened", user_binding.c_str());
    }
  }
  return opened;
}

bool hook_open_pack(void* pak) {
  void** table = *reinterpret_cast<void***>(pak);
  void** entry = table + kOpenPackNamed / sizeof(void*);
  DWORD old = 0;
  if (!VirtualProtect(entry, sizeof(void*), PAGE_READWRITE, &old)) {
    return false;
  }
  original_open_pack = reinterpret_cast<OpenPackNamed>(*entry);
  *entry = reinterpret_cast<void*>(&hooked_open_pack);
  DWORD unused = 0;
  VirtualProtect(entry, sizeof(void*), old, &unused);
  return true;
}

// The marker pack: Levels/<level>/level.pak for every custom map (an empty file; the scan only
// asks whether it is there), written to the temp folder.
std::string write_marker(const std::vector<std::string>& levels) {
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  for (const std::string& level : levels) {
    entries.emplace_back("Levels/" + level + "/level.pak", std::vector<std::uint8_t>{0});
  }
  const std::vector<std::uint8_t> bytes = files::stored_zip_entries(entries);
  wchar_t temp[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  const std::filesystem::path folder = std::filesystem::path(temp) / L"kcds-maps";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  // Earlier games' marker packs; one a running game has open is locked and stays.
  if (markers == 0) {
    std::vector<std::filesystem::path> earlier;
    for (const auto& old : std::filesystem::directory_iterator(folder, error)) {
      earlier.push_back(old.path());
    }
    for (const auto& old : earlier) {
      std::error_code ignored;
      std::filesystem::remove(old, ignored);
    }
  }
  ++markers;
  const std::filesystem::path file = folder / (L"maps-" + std::to_wstring(GetCurrentProcessId()) +
                                               L"-" + std::to_wstring(markers) + L".pak");
  {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) {
      return {};
    }
  }
  std::string narrow;
  for (const wchar_t c : file.wstring()) {
    if (c > 0x7F) {
      return {};
    }
    narrow += char(c);
  }
  return narrow;
}

// Lists levels as the game's (a marker pack for them) and hooks the level pack's opening the first
// time; engine thread.
bool list(const std::vector<std::string>& levels) {
  if (!crypak || levels.empty()) {
    return false;
  }
  const std::string marker = write_marker(levels);
  const bool opened =
      !marker.empty() && slot<OpenPacksBound>(crypak, kOpenPacksBound)(
                             crypak, pak_root.c_str(), marker.c_str(), kModPakFlags, nullptr);
  const bool hooked = opened && (original_open_pack || hook_open_pack(crypak));
  if (hooked) {
    std::lock_guard<std::mutex> lock(maps_mutex);
    found.insert(levels.begin(), levels.end());
  }
  if (log_line) {
    log_line("MAPS %zu custom map(s) %s, level pack hook %s (first %s)", levels.size(),
             opened ? "listed" : "NOT listed", hooked ? "on" : "off", levels.front().c_str());
  }
  return hooked;
}

// Has the level system scan its levels again (CLevelSystem::Rescan), once its slot is the
// function names; engine thread.
bool rescan(void* framework) {
  void* level_system = nullptr;
  if (framework) {
    level_system =
        *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + kLevelSystemAt);
  }
  const HMODULE game = GetModuleHandleW(L"WHGame.dll");
  if (!level_system || !game) {
    return false;
  }
  void* function = (*reinterpret_cast<void***>(level_system))[kRescanSlot];
  const std::uintptr_t expected = reinterpret_cast<std::uintptr_t>(game) + kRescanRva;
  if (reinterpret_cast<std::uintptr_t>(function) != expected) {
    if (log_line) {
      log_line("MAPS level system rescan not found (slot %zu is not 1817DF5F8)", kRescanSlot);
    }
    return false;
  }
  reinterpret_cast<Rescan>(function)(level_system, "levels", kMainLevelsTag);
  return true;
}

// Writes a map's file from a server into its folder (through a temporary file); true when the
// folder holds those bytes. A pack the game has open stays as it is.
bool keep(const std::string& level, const std::string& name,
          const std::vector<std::uint8_t>& bytes) {
  const std::filesystem::path folder = maps_folder() / level;
  const std::filesystem::path file = folder / name;
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  {
    std::ifstream in(file, std::ios::binary);
    const std::vector<std::uint8_t> held((std::istreambuf_iterator<char>(in)),
                                         std::istreambuf_iterator<char>());
    if (held == bytes) {
      return true;
    }
  }
  const std::filesystem::path part = folder / (name + ".part");
  {
    std::ofstream out(part, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
    if (!out) {
      return false;
    }
  }
  std::filesystem::rename(part, file, error);
  if (error) {
    std::filesystem::remove(part, error);
    return false;
  }
  return true;
}

double number_of(const kcd2s::sdk::Value& message, const char* name) {
  const kcd2s::sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (field && field->tag() == kcd2s::sdk::Tag::Int) {
    return double(field->as_int());
  }
  if (field && field->tag() == kcd2s::sdk::Tag::Double) {
    return field->as_double();
  }
  return -1.0;
}

std::string text_of(const kcd2s::sdk::Value& message, const char* name) {
  const kcd2s::sdk::Value* field = kcd2s::integration::script_field(message, name);
  if (field && field->tag() == kcd2s::sdk::Tag::Str) {
    return field->as_str();
  }
  return {};
}

// A custom map's size and ground height from its map.json (the generator's), or the defaults.
void size_of(const std::string& level, float& size, float& height) {
  size = 1024.0f;
  height = 32.0f;
  std::ifstream in(maps_folder() / level / L"map.json");
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::size_t size_at = text.find("\"size\":");
  const std::size_t height_at = text.find("\"height\":");
  if (size_at != std::string::npos) {
    size = float(std::atof(text.c_str() + size_at + 7));
  }
  if (height_at != std::string::npos) {
    height = float(std::atof(text.c_str() + height_at + 9));
  }
}

}  // namespace

bool is_custom(const std::string& level) {
  const std::string name = lower(level);
  if (name.rfind(kPrefix, 0) != 0) {
    return false;
  }
  const std::string rest = name.substr(std::strlen(kPrefix));
  return !rest.empty() && rest.size() <= kLongestName &&
         std::all_of(rest.begin(), rest.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
         });
}

bool exists(const std::string& level) {
  return is_custom(level) && !pack_path(lower(level)).empty();
}

void mount(void* pak, const char* binding_root, Logger logger) {
  log_line = logger;
  if (!pak || !binding_root) {
    return;
  }
  crypak = pak;
  pak_root = binding_root;
  std::vector<std::string> levels;
  std::error_code error;
  for (const auto& entry : std::filesystem::directory_iterator(maps_folder(), error)) {
    const std::string level = lower(entry.path().filename().string());
    if (entry.is_directory(error) && is_custom(level) && !pack_path(level).empty()) {
      levels.push_back(level);
      take_next_navmesh(level);
    }
  }
  list(levels);
}

int map_size(const std::string& level) {
  const std::string name = lower(level);
  if (!exists(name)) {
    return 0;
  }
  float size = 0.0f;
  float height = 0.0f;
  size_of(name, size, height);
  return int(size);
}

bool keep_next_navmesh(const std::string& level, const std::vector<std::uint8_t>& index,
                       const std::vector<std::uint8_t>& tiles) {
  const std::string name = lower(level);
  if (!exists(name)) {
    return false;
  }
  const std::vector<std::uint8_t> bytes = files::stored_zip_entries(
      {{"Recast/RDNavmesh_1.hdr", index}, {"Recast/RDNavmesh_1.nav", tiles}});
  const std::filesystem::path file = maps_folder() / name / L"recast.next.pak";
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(out);
}

bool listed(const std::string& level) {
  std::lock_guard<std::mutex> lock(maps_mutex);
  return found.count(lower(level)) != 0;
}

std::vector<std::string> names() {
  std::vector<std::string> out;
  {
    std::lock_guard<std::mutex> lock(maps_mutex);
    out.assign(found.begin(), found.end());
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool copy_to(const std::string& level, const std::filesystem::path& folder) {
  const std::string name = lower(level);
  if (!exists(name)) {
    return false;
  }
  std::error_code error;
  std::filesystem::create_directories(folder / name, error);
  for (const wchar_t* file : {L"level.pak", L"map.json"}) {
    std::filesystem::copy_file(maps_folder() / name / file, folder / name / file,
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      return false;
    }
  }
  // Its navmesh, when it has one.
  if (std::filesystem::is_regular_file(maps_folder() / name / L"recast.pak", error)) {
    std::filesystem::copy_file(maps_folder() / name / L"recast.pak", folder / name / L"recast.pak",
                               std::filesystem::copy_options::overwrite_existing, error);
    if (error) {
      return false;
    }
  }
  return true;
}

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  // A map World Architect's AI assistant just made on this PC (the MCP server's new_map): listed
  // by the next frame, as a map a server sent.
  if (text_of(message, "op") == "architectMapAdded") {
    const std::string level = lower(text_of(message, "map"));
    if (is_custom(level) && !pack_path(level).empty()) {
      std::lock_guard<std::mutex> lock(maps_mutex);
      arrived.insert(level);
    }
    return true;
  }
  if (text_of(message, "op") != "customMapFile") {
    return false;
  }
  const std::string level = lower(text_of(message, "map"));
  const std::string name = text_of(message, "name");
  // A big file the runtime left on the disk: copied from the download cache,
  // when it is one of this server's files.
  const std::string cached = text_of(message, "cached");
  if (!cached.empty()) {
    const bool known = name == "level.pak" || name == "map.json" || name == "recast.pak";
    const std::filesystem::path source = kcds::runtime::session::cached_resource_file(cached);
    bool copied = false;
    if (is_custom(level) && known && !source.empty()) {
      const std::filesystem::path folder = maps_folder() / level;
      std::error_code error;
      std::filesystem::create_directories(folder, error);
      copied = std::filesystem::copy_file(source, folder / name,
                                          std::filesystem::copy_options::overwrite_existing, error);
    }
    if (log_line) {
      log_line("MAPS %s: %s from the cache %s", level.c_str(), name.c_str(),
               copied ? "kept" : "NOT kept");
    }
    if (copied && name == "map.json" && !pack_path(level).empty()) {
      std::lock_guard<std::mutex> lock(maps_mutex);
      arrived.insert(level);
    }
    return true;
  }
  const double offset = number_of(message, "offset");
  const double total = number_of(message, "total");
  const std::size_t most = name == "map.json" ? kMostAbout : kMostPack;
  const bool known_name = name == "level.pak" || name == "map.json" || name == "recast.pak";
  if (!is_custom(level) || !known_name || total < 1 || total > double(most) || offset < 0 ||
      offset >= total) {
    return true;
  }
  std::vector<std::uint8_t> piece;
  if (!files::decode_base64(text_of(message, "data"), piece)) {
    return true;
  }
  const std::string key = level + "/" + name;
  std::vector<std::uint8_t> complete;
  {
    std::lock_guard<std::mutex> lock(maps_mutex);
    Incoming& file = incoming[key];
    const std::size_t begin = std::size_t(offset);
    if (begin == 0) {
      file.bytes.assign(std::size_t(total), 0);
      file.received = 0;
    }
    if (file.bytes.size() != std::size_t(total) || begin + piece.size() > file.bytes.size()) {
      incoming.erase(key);
      return true;
    }
    std::copy(piece.begin(), piece.end(), file.bytes.begin() + std::ptrdiff_t(begin));
    file.received += piece.size();
    if (file.received < file.bytes.size()) {
      return true;
    }
    complete = std::move(file.bytes);
    incoming.erase(key);
  }
  const bool kept = keep(level, name, complete);
  if (log_line) {
    log_line("MAPS %s: %s from the server %s (%zu bytes)", level.c_str(), name.c_str(),
             kept ? "kept" : "NOT kept (in use?)", complete.size());
  }
  // map.json comes last: the map is complete.
  if (name == "map.json" && !pack_path(level).empty()) {
    std::lock_guard<std::mutex> lock(maps_mutex);
    arrived.insert(level);
  }
  return true;
}

void frame(void* framework) {
  std::vector<std::string> levels;
  {
    std::lock_guard<std::mutex> lock(maps_mutex);
    for (const std::string& level : arrived) {
      if (found.count(level) == 0) {
        levels.push_back(level);
      }
    }
    arrived.clear();
  }
  if (levels.empty()) {
    return;
  }
  const bool listed_now = list(levels);
  const bool rescanned = listed_now && rescan(framework);
  if (log_line) {
    log_line("MAPS %zu new map(s) listed=%d levels scanned again=%d", levels.size(),
             int(listed_now), int(rescanned));
  }
}

void stage_player(const std::string& level, void* local_entity) {
  static std::string staged;
  if (!local_entity || !is_custom(level) || staged == level) {
    return;
  }
  staged = level;
  const float* current = slot<WorldMatrix>(local_entity, 32 * sizeof(void*))(local_entity);
  if (!current) {
    return;
  }
  float size = 0.0f;
  float height = 0.0f;
  size_of(lower(level), size, height);
  float matrix[12];
  std::memcpy(matrix, current, sizeof(matrix));
  matrix[3] = size / 2.0f;
  matrix[7] = size / 2.0f;
  matrix[11] = height + 1.0f;
  slot<SetWorldMatrix>(local_entity, 30 * sizeof(void*))(local_entity, matrix, kSetWorldPosReason);
  if (log_line) {
    log_line("MAPS %s: the player put in the middle (%.0f %.0f %.1f)", level.c_str(), matrix[3],
             matrix[7], matrix[11]);
  }
}

}  // namespace kcds::runtime::maps::custom
