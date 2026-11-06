#include "startup_tables.hpp"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

#include "../files/stored_zip.hpp"
#include "MinHook.h"

namespace kcds::runtime::startup_tables {
namespace {

// ICryPak OpenPacks (by byte offset in its vtable) and the flags the engine opens a mod's packs
// with (WHGame 1.5.5, as branding/runtime_branding.cpp mounts KCDS's own pack).
constexpr std::size_t kOpenPacksBound = 0x48;
constexpr unsigned kModPakFlags = 0x10400;
using OpenPacksBound = bool(__fastcall*)(void* pak, const char* binding_root, const char* wildcard,
                                         unsigned flags, void* full_paths);
constexpr std::size_t kLongestFolder = 64;
constexpr const char* kPerkTables = "libs/tables/rpg/";
constexpr const wchar_t* kLastServer = L"last-server.txt";
constexpr const char* kRestartNotice =
    "This server has perks of its own. Restart the game to have them.";

Logger log_line = nullptr;
Notify notify_player = nullptr;
std::mutex state_mutex;
// The server whose patches this start mounted, and those patches (game path, lower case -> text).
std::string mounted_server;
std::map<std::string, std::string> mounted_files;
// The server being joined, and whether the player was told to restart for it.
std::string current_server;
bool told = false;

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return char(std::tolower(c)); });
  return text;
}

// A game path in one form: forward slashes, lower case.
std::string normal_path(const std::string& path) {
  std::string out = lower(path);
  std::replace(out.begin(), out.end(), '\\', '/');
  return out;
}

std::filesystem::path base_folder() {
  wchar_t local[MAX_PATH] = {};
  const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(local) / L"KCDS" / L"startup-tables";
}

std::string environment(const wchar_t* name) {
  wchar_t value[256] = {};
  const DWORD length = GetEnvironmentVariableW(name, value, 256);
  if (length == 0 || length >= 256) {
    return {};
  }
  std::string out;
  for (DWORD index = 0; index < length; ++index) {
    if (value[index] > 0x7F) {
      return {};
    }
    out += char(value[index]);
  }
  return out;
}

std::string read_text(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// The server this start is for: the launcher's join target, else the last server joined, else the
// launcher's configured server.
std::string start_server(const std::filesystem::path& base) {
  std::string folder = server_folder(environment(L"KCDS_JOIN_ADDRESS"));
  if (folder.empty()) {
    folder = server_folder(read_text(base / kLastServer));
  }
  if (folder.empty()) {
    folder = server_folder(environment(L"KCDS_SERVER_ADDRESS"));
  }
  return folder;
}

// A path as a narrow string CryPak opens (plain ASCII only), empty for another.
std::string narrow(const std::filesystem::path& path) {
  std::string out;
  for (const wchar_t c : path.wstring()) {
    if (c > 0x7F) {
      return {};
    }
    out += char(c);
  }
  return out;
}

// The game's table patch listing (a table's path, the list to fill, its mods): every
// table type has its own copy of the load (is one of about 80), and every copy lists the
// table's <Table>__*.xml patches here, through the game's CryPak. The game reads nearly all its
// tables before the engine mounts the mods' packs, so the pack is mounted at the
// first listing, through that CryPak (its global), bound where a mod's packs are.
using PatchList = std::intptr_t(__fastcall*)(void* table, void* list, void* mods);
PatchList original_patch_list = nullptr;
void* const* crypak_global = nullptr;
constexpr const char* kDataRoot = "data";

std::intptr_t __fastcall first_patch_list(void* table, void* list, void* mods) {
  if (crypak_global && *crypak_global) {
    mount(*crypak_global, kDataRoot, log_line);
  }
  // The table's path (a CryString), read before the listing takes it.
  const char* path = table ? *static_cast<const char* const*>(table) : nullptr;
  const std::string name = path ? lower(path) : std::string();
  const std::intptr_t result = original_patch_list(table, list, mods);
  // The perk table's patches as the game lists them (a std::set of CryStrings: head node, size;
  // a node's left +0, right +16, is-nil +25, value +32), in the log.
  if (log_line && name.find("rpg\\perk.xml") != std::string::npos && list) {
    auto* head = *static_cast<std::uint8_t* const*>(list);
    const std::size_t size =
        *reinterpret_cast<const std::size_t*>(static_cast<std::uint8_t*>(list) + 8);
    std::string names;
    std::vector<std::uint8_t*> stack;
    std::uint8_t* node = head ? *reinterpret_cast<std::uint8_t**>(head + 8) : nullptr;
    while ((node && !node[25]) || !stack.empty()) {
      while (node && !node[25]) {
        stack.push_back(node);
        node = *reinterpret_cast<std::uint8_t**>(node);
      }
      node = stack.back();
      stack.pop_back();
      const char* text = *reinterpret_cast<const char* const*>(node + 32);
      names += text ? text : "?";
      names += ' ';
      node = *reinterpret_cast<std::uint8_t**>(node + 16);
    }
    log_line("STARTUP TABLES %s: %zu patch(es): %s", name.c_str(), size, names.c_str());
  }
  return result;
}

}  // namespace

void hook_table_loads(void* listing, void* const* crypak, Logger logger) {
  if (!log_line) {
    log_line = logger;
  }
  if (!listing || !crypak || original_patch_list) {
    return;
  }
  crypak_global = crypak;
  const bool hooked = MH_CreateHook(listing, reinterpret_cast<void*>(&first_patch_list),
                                    reinterpret_cast<void**>(&original_patch_list)) == MH_OK &&
                      MH_EnableHook(listing) == MH_OK;
  if (log_line) {
    log_line("STARTUP TABLES the game's table patch listing hooked=%u", unsigned(hooked));
  }
}

void install(Logger logger, Notify notify) {
  log_line = logger;
  notify_player = notify;
}

std::string server_folder(const std::string& address) {
  std::string out;
  for (const char raw : address) {
    const char c = char(std::tolower(static_cast<unsigned char>(raw)));
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '-') {
      out += c;
    } else if (c == ':') {
      out += '_';
    } else if (c == '\r' || c == '\n' || c == ' ') {
      continue;
    } else {
      return {};
    }
  }
  if (out.size() > kLongestFolder || out.find("..") != std::string::npos) {
    return {};
  }
  return out;
}

bool is_perk_patch(const std::string& path) {
  const std::string game_path = normal_path(path);
  if (game_path.rfind(kPerkTables, 0) != 0) {
    return false;
  }
  const std::string name = game_path.substr(std::string(kPerkTables).size());
  const bool xml = name.size() > 4 && name.compare(name.size() - 4, 4, ".xml") == 0;
  return name.rfind("perk", 0) == 0 && name.find("__") != std::string::npos &&
         name.find('/') == std::string::npos && xml;
}

void mount(void* pak, const char* binding_root, Logger logger) {
  if (!log_line) {
    log_line = logger;
  }
  // Once a start: at the game's first table load, else after the mods' packs.
  static bool attempted = false;
  if (attempted) {
    return;
  }
  attempted = true;
  const std::filesystem::path base = base_folder();
  if (!pak || !binding_root || base.empty()) {
    return;
  }
  const std::string folder = start_server(base);
  if (folder.empty()) {
    return;
  }
  const std::filesystem::path kept = base / folder;
  std::vector<std::pair<std::string, std::vector<std::uint8_t>>> entries;
  std::map<std::string, std::string> files;
  std::error_code error;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(kept, error)) {
    if (!entry.is_regular_file(error)) {
      continue;
    }
    const std::string game_path =
        std::filesystem::relative(entry.path(), kept, error).generic_string();
    if (error || !is_perk_patch(game_path)) {
      continue;
    }
    const std::string text = read_text(entry.path());
    entries.emplace_back(game_path, std::vector<std::uint8_t>(text.begin(), text.end()));
    files[normal_path(game_path)] = text;
  }
  if (entries.empty()) {
    return;
  }
  const std::filesystem::path pack = base / (folder + ".pak");
  {
    const std::vector<std::uint8_t> bytes = files::stored_zip_entries(entries);
    std::ofstream out(pack, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  }
  const std::string pack_path = narrow(pack);
  const auto open_packs = reinterpret_cast<OpenPacksBound>(
      (*reinterpret_cast<void***>(pak))[kOpenPacksBound / sizeof(void*)]);
  const bool opened =
      !pack_path.empty() && open_packs(pak, binding_root, pack_path.c_str(), kModPakFlags, nullptr);
  if (opened) {
    std::lock_guard<std::mutex> lock(state_mutex);
    mounted_server = folder;
    mounted_files = std::move(files);
  }
  if (log_line) {
    log_line("STARTUP TABLES %s: %zu perk patch(es) %s from %s at %s", folder.c_str(),
             entries.size(), opened ? "mounted" : "NOT mounted", pack_path.c_str(), binding_root);
  }
}

void joining(const std::string& address) {
  const std::string folder = server_folder(address);
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    current_server = folder;
    told = false;
  }
  const std::filesystem::path base = base_folder();
  if (folder.empty() || base.empty()) {
    return;
  }
  std::error_code error;
  std::filesystem::create_directories(base, error);
  std::ofstream out(base / kLastServer, std::ios::binary | std::ios::trunc);
  out << address;
}

bool keep(const std::string& path, const std::string& text) {
  if (!is_perk_patch(path)) {
    return false;
  }
  std::string folder;
  bool loaded = false;
  bool tell = false;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    folder = current_server;
    const auto mounted = mounted_files.find(normal_path(path));
    loaded = mounted_server == folder && mounted != mounted_files.end() && mounted->second == text;
    tell = !loaded && !told && !folder.empty();
    if (tell) {
      told = true;
    }
  }
  const std::filesystem::path base = base_folder();
  if (folder.empty() || base.empty()) {
    return true;
  }
  const std::filesystem::path file = base / folder / std::filesystem::path(normal_path(path));
  if (read_text(file) != text) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), std::streamsize(text.size()));
  }
  if (log_line) {
    // The perk database's rows (C_PerkDatabase: rows from + 0x30 to + 0x38, 120 bytes
    // each; the game's own tables hold 945), for the log.
    std::size_t rows = 0;
    if (crypak_global) {
      const auto* database =
          reinterpret_cast<const std::uint8_t*>(crypak_global) - (0x18492D850 - 0x18532E150);
      const auto begin = *reinterpret_cast<const std::uintptr_t*>(database + 0x30);
      const auto end = *reinterpret_cast<const std::uintptr_t*>(database + 0x38);
      rows = end > begin ? (end - begin) / 120 : 0;
    }
    log_line("STARTUP TABLES %s: %s kept, %s (the game's perk table has %zu rows)", folder.c_str(),
             path.c_str(), loaded ? "loaded at this start" : "for the next start", rows);
  }
  if (tell && notify_player) {
    notify_player(kRestartNotice);
  }
  return true;
}

}  // namespace kcds::runtime::startup_tables
