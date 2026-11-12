// The client's shared state (its console) first: it brings Windows in with its sockets.
#include "../core/client_internal.hpp"

#include "kcd1_map.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "../files/stored_zip.hpp"
#include "install.hpp"
#include "level_files.hpp"
#include "level_pack.hpp"

namespace kcds::runtime::kcd1 {
namespace {

// ICryPak, by byte offset in its vtable (WHGame 1.5.5), as custom_maps.cpp uses it; ClosePack
// (name, flags) follows the named OpenPack.
constexpr std::size_t kAdjustFileName = 8;
constexpr std::size_t kOpenPackBound = 48;
constexpr std::size_t kOpenPackNamed = 56;
constexpr std::size_t kClosePack = 64;
constexpr std::size_t kOpenPacksBound = 0x48;
// CCryPak's pack list (a vector of 56-byte pack entries, the archive at + 48) and the lock its
// open and close take (at + 240: readers at + 0, writers at + 4, the readers' event at + 8, an
// SRW lock at + 16, its recursion at + 24 and owning thread at + 28).
constexpr std::size_t kPackListBegin = 288;
constexpr std::size_t kPackListEnd = 296;
constexpr std::size_t kPackListLock = 240;
constexpr std::size_t kPackEntrySize = 56;
constexpr std::size_t kPackEntryArchive = 48;
constexpr std::size_t kMostPacks = 4096;
// As the engine opens a mod's paks (the marker pack), and a pack by its real path (KCD1's).
constexpr unsigned kModPakFlags = 0x10400;
constexpr unsigned kRealPathFlags = 0x10000;
constexpr unsigned kSetWorldPosReason = 0x400000u;
// KCD1's packs of models and buildings, opened where they are.
constexpr const wchar_t* kAssetPacks[] = {L"Objects.pak", L"Buildings.pak"};
// KCD2 streams its levels' buildings and plants as hierarchical levels of detail and, while that
// system is on, leaves out the ones a terrain's object tree holds (KCD1's): it is off while the
// KCD1 map is loaded (console variable, ICVar Set(int) at slot 7 as the console sets it).
constexpr const char* kHLodVariable = "wh_e_HLodSystemEnabled";
constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarSetInt = 7;
// Where a player starts at Rataje: KCD1's own player start spot by the village
// (q_counterOffensive_playerStartSpot), a metre up.
constexpr float kStart[3] = {2551.64f, 460.34f, 69.02f};

using AdjustFileName = const char*(__fastcall*)(void* pak, const char* path, char* out,
                                                unsigned flags);
using OpenPackBound = bool(__fastcall*)(void* pak, const char* binding_root, const char* name,
                                        unsigned flags, void* memory, void* full_path);
using OpenPackNamed = bool(__fastcall*)(void* pak, const char* name, unsigned flags, void* memory,
                                        void* full_path);
using ClosePack = bool(__fastcall*)(void* pak, const char* name, unsigned flags);
using OpenPacksBound = bool(__fastcall*)(void* pak, const char* binding_root, const char* wildcard,
                                         unsigned flags, void* full_paths);
using GetCVar = void*(__fastcall*)(void* console, const char* name);
using SetCVarInt = void(__fastcall*)(void* variable, int value);
using WorldMatrix = const float*(__fastcall*)(void* entity);
using SetWorldMatrix = void(__fastcall*)(void* entity, const float* matrix, unsigned reason);

template <class Fn>
Fn slot(void* object, std::size_t offset) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[offset / sizeof(void*)]);
}

Logger log_line = nullptr;
OpenPackNamed previous_open_pack = nullptr;
std::atomic<bool> level_listed{false};
// Set at mount (engine thread), read by the level's opening.
std::filesystem::path kcd1_folder;
std::string pak_root;
// While the KCD1 map is loaded: its packs (to close when another level loads) and whether the
// hierarchical levels of detail were switched off.
std::mutex active_mutex;
std::optional<Packs> built;
std::vector<std::string> open_packs;
bool active = false;

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](char c) { return c == '\\' ? '/' : char(std::tolower(unsigned(c))); });
  return text;
}

bool ends_with(const std::string& text, const std::string& tail) {
  return text.size() >= tail.size() &&
         text.compare(text.size() - tail.size(), tail.size(), tail) == 0;
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

std::uint8_t* field(void* pak, std::size_t offset) {
  return static_cast<std::uint8_t*>(pak) + offset;
}

std::uint8_t* pack_list_begin(void* pak) {
  return *reinterpret_cast<std::uint8_t**>(field(pak, kPackListBegin));
}

std::uint8_t* pack_list_end(void* pak) {
  return *reinterpret_cast<std::uint8_t**>(field(pak, kPackListEnd));
}

// CryPak's pack list lock taken for writing, as its open and close take it: the SRW lock (again
// by the thread that holds it), the writers' count up, then the readers waited for.
void lock_pack_list(void* pak) {
  std::uint8_t* lock = field(pak, kPackListLock);
  auto* owner = reinterpret_cast<volatile DWORD*>(lock + 28);
  auto* recursion = reinterpret_cast<volatile LONG*>(lock + 24);
  const DWORD thread = GetCurrentThreadId();
  if (*owner == thread) {
    *recursion += 1;
  } else {
    AcquireSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock + 16));
    *owner = thread;
  }
  InterlockedIncrement(reinterpret_cast<volatile LONG*>(lock + 4));
  while (*reinterpret_cast<volatile LONG*>(lock) != 0) {
    WaitForSingleObject(*reinterpret_cast<HANDLE*>(lock + 8), INFINITE);
  }
}

void unlock_pack_list(void* pak) {
  std::uint8_t* lock = field(pak, kPackListLock);
  InterlockedDecrement(reinterpret_cast<volatile LONG*>(lock + 4));
  auto* recursion = reinterpret_cast<volatile LONG*>(lock + 24);
  if (*recursion != 0) {
    *recursion -= 1;
  } else {
    *reinterpret_cast<volatile DWORD*>(lock + 28) = 0;
    ReleaseSRWLockExclusive(reinterpret_cast<PSRWLOCK>(lock + 16));
  }
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
// priority), keeping their order, under the list's lock. The entries are moved as they are
// (their strings, vector and archive pointer go with them, nothing is copied or freed).
std::size_t move_to_front(void* pak, const std::vector<void*>& before) {
  lock_pack_list(pak);
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
  if (!moved.empty()) {
    std::memcpy(begin, moved.data(), moved.size());
    std::memcpy(begin + moved.size(), kept.data(), kept.size());
  }
  unlock_pack_list(pak);
  return moved.size() / kPackEntrySize;
}

// The hierarchical levels of detail switched on or off; false when the variable is not there.
bool set_hlods(bool on) {
  void* console = kcds::game_client::console_slot ? *kcds::game_client::console_slot : nullptr;
  void* variable =
      console ? slot<GetCVar>(console, kConsoleGetCVar * sizeof(void*))(console, kHLodVariable)
              : nullptr;
  if (!variable) {
    return false;
  }
  slot<SetCVarInt>(variable, kCVarSetInt * sizeof(void*))(variable, on ? 1 : 0);
  return true;
}

// KCD1's packs of models and buildings and its built assets pack (materials, world textures,
// props) at the game's root, behind all of KCD2's; the names of those opened.
std::vector<std::string> open_assets(void* pak, const std::filesystem::path& assets) {
  std::vector<std::filesystem::path> files;
  for (const wchar_t* name : kAssetPacks) {
    std::error_code error;
    if (std::filesystem::is_regular_file(kcd1_folder / L"Data" / name, error)) {
      files.push_back(kcd1_folder / L"Data" / name);
    }
  }
  files.push_back(assets);
  std::vector<std::string> opened;
  const std::vector<void*> before = pack_archives(pak);
  if (before.empty()) {
    if (log_line) {
      log_line("KCD1 CryPak's pack list not found: KCD1's models NOT opened");
    }
    return opened;
  }
  for (const auto& file : files) {
    const std::string path = narrow(file);
    const bool ok =
        !path.empty() && slot<OpenPackBound>(pak, kOpenPackBound)(
                             pak, pak_root.c_str(), path.c_str(), kRealPathFlags, nullptr, nullptr);
    if (ok) {
      opened.push_back(path);
    }
    if (log_line) {
      log_line("KCD1 pack %s %s", ok ? "opened" : "NOT opened", utf8(file).c_str());
    }
  }
  const std::size_t moved = move_to_front(pak, before);
  if (log_line) {
    log_line("KCD1 %zu of %zu packs opened, %zu moved behind the game's own", opened.size(),
             files.size(), moved);
  }
  return opened;
}

// The KCD1 map is about to load: its packs built when needed, opened, and the hierarchical
// levels of detail off. Nothing of it runs before (a game that never loads the map pays nothing).
bool activate(void* pak) {
  std::lock_guard<std::mutex> lock(active_mutex);
  if (active) {
    return built.has_value();
  }
  if (!built) {
    built = build_packs(kcd1_folder, game_folder(), log_line);
  }
  if (!built || narrow(built->level).empty()) {
    if (log_line) {
      log_line("KCD1 the map's packs could not be built: the level does not load");
    }
    return false;
  }
  open_packs = open_assets(pak, built->assets);
  const bool hlods_off = set_hlods(false);
  active = true;
  if (log_line) {
    log_line("KCD1 map active (hierarchical levels of detail %s)",
             hlods_off ? "off" : "NOT switched off");
  }
  return true;
}

// Another level loads: KCD1's packs closed and the hierarchical levels of detail on again.
void deactivate(void* pak) {
  std::lock_guard<std::mutex> lock(active_mutex);
  if (!active) {
    return;
  }
  std::size_t closed = 0;
  for (const std::string& path : open_packs) {
    closed += slot<ClosePack>(pak, kClosePack)(pak, path.c_str(), kRealPathFlags) ? 1 : 0;
  }
  open_packs.clear();
  set_hlods(true);
  active = false;
  if (log_line) {
    log_line("KCD1 map inactive: %zu packs closed, hierarchical levels of detail on", closed);
  }
}

// A level's opening: the KCD1 map's is its built level pack, bound at the level's folder, with
// the map activated first; another level's deactivates the map, then opens as the game (or the
// hook before this one) opens it.
// A pack opened in place of one of the KCD1 map's (`what` in the log), bound at the level's folder.
bool open_at_level(void* pak, const std::string& file, const char* what, unsigned flags,
                   void* memory, void* full_path) {
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
                              pak, binding.c_str(), file.c_str(), flags, memory, full_path);
  if (log_line) {
    log_line("KCD1 %s %s from %s at %s", what, opened ? "opened" : "NOT opened", file.c_str(),
             binding.c_str());
  }
  return opened;
}

bool __fastcall hooked_open_pack(void* pak, const char* name, unsigned flags, void* memory,
                                 void* full_path) {
  const std::string path = name ? lower(name) : std::string();
  const bool ours = ends_with(path, std::string("levels/") + kLevel + "/level.pak");
  if (!ours) {
    if (ends_with(path, "/level.pak") && path.find("levels/") != std::string::npos) {
      deactivate(pak);
    }
    return previous_open_pack(pak, name, flags, memory, full_path);
  }
  if (!activate(pak)) {
    return false;
  }
  // The navigation mesh is KCD1's own (Recast/Detour, the same files in both games): the game
  // reads the level's recast/RDNavmesh_<n>.nav and .hdr as files, so KCD1's recast.pak is opened
  // at the level's folder too, and closed with the map's other packs.
  const std::string navigation = narrow(kcd1_folder / "Data" / "Levels" / "rataje" / "recast.pak");
  if (open_at_level(pak, navigation, "navigation pack", flags, nullptr, nullptr)) {
    std::lock_guard<std::mutex> lock(active_mutex);
    open_packs.push_back(navigation);
  }
  return open_at_level(pak, narrow(built->level), "level pack", flags, memory, full_path);
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
  // Only the level is listed here: its packs are built and opened when it loads (activate).
  kcd1_folder = find_install();
  if (kcd1_folder.empty()) {
    if (log_line) {
      log_line("KCD1 Kingdom Come: Deliverance is not on this PC: no KCD1 map");
    }
    return;
  }
  pak_root = binding_root;
  const std::string marker = write_marker();
  const bool marked =
      !marker.empty() && slot<OpenPacksBound>(pak, kOpenPacksBound)(
                             pak, binding_root, marker.c_str(), kModPakFlags, nullptr);
  const bool hooked = marked && hook_open_pack(pak);
  level_listed.store(hooked);
  if (log_line) {
    log_line("KCD1 Kingdom Come: Deliverance at %s, map %s %s (level pack hook %s)",
             utf8(kcd1_folder).c_str(), kLevel, marked ? "listed" : "NOT listed",
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
