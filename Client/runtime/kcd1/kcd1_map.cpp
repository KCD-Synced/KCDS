#include "kcd1_map.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "../files/stored_zip.hpp"
#include "install.hpp"
#include "level_files.hpp"
#include "level_pack.hpp"

namespace kcds::runtime::kcd1 {
namespace {

// ICryPak, by byte offset in its vtable (WHGame 1.5.5), as custom_maps.cpp uses it.
constexpr std::size_t kAdjustFileName = 8;
constexpr std::size_t kOpenPackBound = 48;
constexpr std::size_t kOpenPackNamed = 56;
constexpr std::size_t kOpenPacksBound = 0x48;
// CCryPak's pack list (a vector of 56-byte pack entries, the archive at + 48), as its inner
// open walks and fills it.
constexpr std::size_t kPackListBegin = 288;
constexpr std::size_t kPackListEnd = 296;
constexpr std::size_t kPackEntrySize = 56;
constexpr std::size_t kPackEntryArchive = 48;
constexpr std::size_t kMostPacks = 4096;
// As the engine opens a mod's paks (the marker pack), and a pack by its real path (KCD1's).
constexpr unsigned kModPakFlags = 0x10400;
constexpr unsigned kRealPathFlags = 0x10000;
constexpr unsigned kSetWorldPosReason = 0x400000u;
// KCD1's packs of models, buildings and textures, opened where they are.
constexpr const wchar_t* kAssetPacks[] = {L"Objects.pak", L"Buildings.pak"};
// Where a player starts at Rataje: KCD1's own player start spot by the village
// (q_counterOffensive_playerStartSpot), a metre up.
constexpr float kStart[3] = {2551.64f, 460.34f, 69.02f};

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

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

Logger log_line = nullptr;
OpenPackNamed previous_open_pack = nullptr;
// The built level pack as a narrow path, set once at mount.
std::string level_pack;
std::atomic<bool> level_listed{false};

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](char c) { return c == '\\' ? '/' : char(std::tolower(unsigned(c))); });
  return text;
}

// A path as CryPak takes it (plain ASCII only), empty for none.
std::string narrow(const std::filesystem::path& file) {
  std::string out;
  for (const wchar_t c : file.wstring()) {
    if (c > 0x7F) {
      return {};
    }
    out += char(c);
  }
  return out;
}

// This game's folder: ...\KingdomComeDeliverance2\Bin\Win64...\KingdomCome.exe -> its first.
std::filesystem::path game_folder() {
  wchar_t executable[MAX_PATH] = {};
  const DWORD length = GetModuleFileNameW(nullptr, executable, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  return std::filesystem::path(executable).parent_path().parent_path().parent_path();
}

std::uint8_t* pack_list_begin(void* pak) {
  return *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(pak) + kPackListBegin);
}

std::uint8_t* pack_list_end(void* pak) {
  return *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(pak) + kPackListEnd);
}

// The archives of CryPak's pack list, in its order; empty when the list does not look like one.
std::vector<void*> pack_archives(void* pak) {
  std::vector<void*> archives;
  std::uint8_t* begin = pack_list_begin(pak);
  std::uint8_t* end = pack_list_end(pak);
  if (!begin || end < begin || std::size_t(end - begin) % kPackEntrySize != 0 ||
      std::size_t(end - begin) / kPackEntrySize > kMostPacks) {
    return archives;
  }
  for (std::uint8_t* entry = begin; entry < end; entry += kPackEntrySize) {
    archives.push_back(*reinterpret_cast<void**>(entry + kPackEntryArchive));
  }
  return archives;
}

// Moves the packs whose archives are not in `before` to the front of CryPak's list (its lowest
// priority), keeping their order. The entries are moved as they are (their strings, vector and
// archive pointer go with them, nothing is copied or freed). Engine thread, at startup.
std::size_t move_to_front(void* pak, const std::vector<void*>& before) {
  std::uint8_t* begin = pack_list_begin(pak);
  const std::vector<void*> now = pack_archives(pak);
  std::vector<std::uint8_t> moved;
  std::vector<std::uint8_t> kept;
  for (std::size_t index = 0; index < now.size(); ++index) {
    std::uint8_t* entry = begin + index * kPackEntrySize;
    const bool added = std::find(before.begin(), before.end(), now[index]) == before.end();
    std::vector<std::uint8_t>& into = added ? moved : kept;
    into.insert(into.end(), entry, entry + kPackEntrySize);
  }
  if (moved.empty()) {
    return 0;
  }
  std::memcpy(begin, moved.data(), moved.size());
  std::memcpy(begin + moved.size(), kept.data(), kept.size());
  return moved.size() / kPackEntrySize;
}

// KCD1's packs of models and buildings and its built assets pack (materials, world textures) at
// the game's root, behind all of KCD2's.
void open_assets(void* pak, const char* binding_root, const std::filesystem::path& kcd1,
                 const std::filesystem::path& assets) {
  std::vector<std::filesystem::path> files;
  for (const wchar_t* name : kAssetPacks) {
    std::error_code error;
    if (std::filesystem::is_regular_file(kcd1 / L"Data" / name, error)) {
      files.push_back(kcd1 / L"Data" / name);
    }
  }
  files.push_back(assets);
  const std::vector<void*> before = pack_archives(pak);
  if (before.empty()) {
    if (log_line) {
      log_line("KCD1 CryPak's pack list not found: KCD1's models NOT opened");
    }
    return;
  }
  std::size_t opened = 0;
  for (const auto& file : files) {
    const std::string path = narrow(file);
    const bool ok =
        !path.empty() && slot<OpenPackBound>(pak, kOpenPackBound)(pak, binding_root, path.c_str(),
                                                                  kRealPathFlags, nullptr, nullptr);
    opened += ok ? 1 : 0;
    if (log_line) {
      log_line("KCD1 pack %s %s", ok ? "opened" : "NOT opened", utf8(file).c_str());
    }
  }
  const std::size_t moved = move_to_front(pak, before);
  if (log_line) {
    log_line("KCD1 %zu of %zu packs opened, %zu moved behind the game's own (%zu packs)", opened,
             files.size(), moved, before.size() + opened);
  }
}

// The level's opening: the built level pack, bound at the level's folder; any other pack as the
// game (or the hook before this one) opens it.
bool __fastcall hooked_open_pack(void* pak, const char* name, unsigned flags, void* memory,
                                 void* full_path) {
  const std::string path = name ? lower(name) : std::string();
  const std::string wanted = std::string("levels/") + kLevel + "/level.pak";
  const bool ours = !level_pack.empty() && path.size() >= wanted.size() &&
                    path.compare(path.size() - wanted.size(), wanted.size(), wanted) == 0;
  if (!ours) {
    return previous_open_pack(pak, name, flags, memory, full_path);
  }
  // The folder as a lookup adjusts it (the game's data folder in front), the pack's root.
  const std::string folder = std::string("levels/") + kLevel + "/";
  char adjusted[2048] = {};
  const char* root = slot<AdjustFileName>(pak, kAdjustFileName)(pak, folder.c_str(), adjusted, 0);
  std::string binding = root ? std::string(root) : std::string();
  if (!binding.empty() && binding.back() != '\\' && binding.back() != '/') {
    binding += '\\';
  }
  const bool opened =
      !binding.empty() && slot<OpenPackBound>(pak, kOpenPackBound)(
                              pak, binding.c_str(), level_pack.c_str(), flags, memory, full_path);
  if (log_line) {
    log_line("KCD1 level pack %s from %s at %s", opened ? "opened" : "NOT opened",
             level_pack.c_str(), binding.c_str());
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
  previous_open_pack = reinterpret_cast<OpenPackNamed>(*entry);
  *entry = reinterpret_cast<void*>(&hooked_open_pack);
  DWORD unused = 0;
  VirtualProtect(entry, sizeof(void*), old, &unused);
  return true;
}

// The marker pack (Levels/kcd1_rataje/level.pak, one byte: the level system's scan only asks
// whether it is there), written to the temp folder.
std::string write_marker() {
  const std::vector<std::uint8_t> bytes = files::stored_zip_entries(
      {{std::string("Levels/") + kLevel + "/level.pak", std::vector<std::uint8_t>{0}}});
  wchar_t temp[MAX_PATH] = {};
  const DWORD length = GetTempPathW(MAX_PATH, temp);
  if (length == 0 || length >= MAX_PATH) {
    return {};
  }
  const std::filesystem::path folder = std::filesystem::path(temp) / L"kcds-kcd1";
  std::error_code error;
  std::filesystem::create_directories(folder, error);
  const std::filesystem::path file =
      folder / (L"marker-" + std::to_wstring(GetCurrentProcessId()) + L".pak");
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return out ? narrow(file) : std::string();
}

}  // namespace

bool is_level(const std::string& level) {
  return lower(level) == kLevel;
}

bool listed() {
  return level_listed.load();
}

void mount(void* pak, const char* binding_root, Logger logger) {
  log_line = logger;
  if (!pak || !binding_root) {
    return;
  }
  const std::filesystem::path kcd1 = find_install();
  if (kcd1.empty()) {
    if (log_line) {
      log_line("KCD1 Kingdom Come: Deliverance is not on this PC: no KCD1 map");
    }
    return;
  }
  if (log_line) {
    log_line("KCD1 Kingdom Come: Deliverance found at %s", utf8(kcd1).c_str());
  }
  const std::optional<Packs> packs = build_packs(kcd1, game_folder(), log_line);
  if (!packs) {
    return;
  }
  level_pack = narrow(packs->level);
  if (level_pack.empty()) {
    if (log_line) {
      log_line("KCD1 the level pack's path is not plain ASCII: no KCD1 map");
    }
    return;
  }
  open_assets(pak, binding_root, kcd1, packs->assets);
  const std::string marker = write_marker();
  const bool marked =
      !marker.empty() && slot<OpenPacksBound>(pak, kOpenPacksBound)(
                             pak, binding_root, marker.c_str(), kModPakFlags, nullptr);
  const bool hooked = marked && hook_open_pack(pak);
  level_listed.store(hooked);
  if (log_line) {
    log_line("KCD1 map %s %s (level pack hook %s)", kLevel, marked ? "listed" : "NOT listed",
             hooked ? "on" : "off");
  }
}

void stage_player(const std::string& level, void* local_entity) {
  if (!local_entity || !is_level(level)) {
    return;
  }
  const float* current = slot<WorldMatrix>(local_entity, 32 * sizeof(void*))(local_entity);
  if (!current) {
    return;
  }
  float matrix[12];
  std::memcpy(matrix, current, sizeof(matrix));
  matrix[3] = kStart[0];
  matrix[7] = kStart[1];
  matrix[11] = kStart[2];
  slot<SetWorldMatrix>(local_entity, 30 * sizeof(void*))(local_entity, matrix, kSetWorldPosReason);
  if (log_line) {
    log_line("KCD1 the player put at Rataje (%.0f %.0f %.1f)", matrix[3], matrix[7], matrix[11]);
  }
}

}  // namespace kcds::runtime::kcd1
