// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "studio_scene.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "../assets/asset_container.hpp"
#include "../files/zip_read.hpp"
#include "studio_level.hpp"
#include "../assets/memory_paks.hpp"
#include "../core/log.hpp"
#include "../identity/hardware_id.hpp"
#include "../core/natives.hpp"
#include "../world/world_clock.hpp"
#include "../world/world_weather.hpp"
#include "studio_floor.hpp"

namespace kcds::runtime::studio {
namespace {

// The middle of the floor's first sector (make_studio_level.py: FLOOR_HEIGHT 32, sectors of
// 64 m; the game draws the sector from 0 to 64 m, the body at 64 stood on its corner), a little
// above it so the body lands.
constexpr float kStageX = 32.0f;
constexpr float kStageY = 32.0f;
constexpr float kStageZ = 32.1f;

// The studio's light: noon of the debug start's own day (it starts at day 1, 9:00), the clock
// stopped, no clouds.
constexpr std::uint32_t kStudioDay = 1;
constexpr float kStudioHour = 12.0f;
constexpr const char* kStudioWeather = "cloudless_sunny";

// The game lists its levels when it starts (CLevelSystem scan): each folder of
// Data/Levels with a level.pak on disk (CCryPak vtable + 536 with "on disk") is a level. So the
// studio's folder holds an empty zip of 22 bytes; the level itself comes from memory when the
// game opens that pak (assets/memory_paks).
// The game's folder (...\Bin\Win64MasterMasterSteamPGO\KingdomCome.exe -> ...); empty when the
// executable's path has fewer folders.
std::wstring game_folder() {
  wchar_t module[MAX_PATH] = {};
  GetModuleFileNameW(nullptr, module, MAX_PATH);
  std::wstring folder(module);
  for (int up = 0; up < 3; ++up) {
    const auto slash = folder.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
      return {};
    }
    folder.resize(slash);
  }
  return folder;
}

// The studio's level made from the player's own klaster level (studio_level.hpp), read from its
// level.pak on disk only where needed; nullopt when that pak is missing or does not read.
std::optional<std::vector<std::uint8_t>> level_from_game_files() {
  const std::wstring folder = game_folder();
  if (folder.empty()) {
    return std::nullopt;
  }
  const std::wstring path = folder + L"\\Data\\Levels\\klaster\\level.pak";
  std::ifstream pak(path, std::ios::binary);
  if (!pak) {
    return std::nullopt;
  }
  pak.seekg(0, std::ios::end);
  const std::uint64_t size = std::uint64_t(pak.tellg());
  const files::ZipReadAt read = [&pak](std::uint64_t offset, std::size_t length,
                                       std::vector<std::uint8_t>& out) {
    out.resize(length);
    pak.clear();
    pak.seekg(std::streamoff(offset));
    if (length == 0) {
      return bool(pak);
    }
    pak.read(reinterpret_cast<char*>(out.data()), std::streamsize(length));
    return std::size_t(pak.gcount()) == length;
  };
  return level::build(read, size);
}

void write_level_stub() {
  std::wstring folder = game_folder();
  if (folder.empty()) {
    return;
  }
  folder += L"\\Data\\Levels\\";
  folder += std::wstring(kLevel, kLevel + std::strlen(kLevel));
  CreateDirectoryW(folder.c_str(), nullptr);
  const std::wstring stub = folder + L"\\level.pak";
  if (GetFileAttributesW(stub.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return;
  }
  // An end-of-central-directory record and nothing else: a zip with no files.
  const std::uint8_t empty_zip[22] = {0x50, 0x4B, 0x05, 0x06};
  std::ofstream out(stub, std::ios::binary);
  out.write(reinterpret_cast<const char*>(empty_zip), sizeof(empty_zip));
  game_client::log("STUDIO level stub written (no game data)");
}

}  // namespace

bool requested() {
  const char* value = std::getenv("KCDS_ASSET_STUDIO");
  return value && std::string(value) == "1";
}

void prepare_level_listing() {
  write_level_stub();
}

bool provide_level() {
  // A sealed copy made on this machine (Dev/tools/assets/kcds_studio, kcds-asset-seal
  // --this-machine) when there is one: a developer's own.
  std::optional<std::vector<std::uint8_t>> pak;
  const char* local = std::getenv("LOCALAPPDATA");
  const std::string path =
      std::string(local ? local : "") + "\\KCDS\\studio\\kcds_studio.kcdsasset";
  std::ifstream file(path, std::ios::binary);
  if (local && file) {
    const std::vector<std::uint8_t> sealed((std::istreambuf_iterator<char>(file)),
                                           std::istreambuf_iterator<char>());
    const std::string secret = "kcds-studio-local:" + identity::hardware_id();
    pak = assets::container::open(sealed, kLevel, secret);
    if (!pak) {
      game_client::log("STUDIO level %s does not open (another machine's, or changed)",
                       path.c_str());
    }
  }
  // Otherwise (every player) the level is made now from the player's own game files.
  if (!pak) {
    pak = level_from_game_files();
    if (!pak) {
      game_client::log(
          "STUDIO level could not be made: the game's klaster level.pak is missing "
          "or does not read");
      return false;
    }
    game_client::log("STUDIO level made from the game's files, %zu bytes", pak->size());
  } else {
    game_client::log("STUDIO level opened in memory, %zu bytes", pak->size());
  }
  assets::memory_paks::provide(std::string("levels/") + kLevel + "/level.pak", std::move(*pak));
  write_level_stub();
  return true;
}

namespace {

// Once per studio world: the player put in the middle, the clock set and the sky held (leave
// makes them due again).
bool staged = false;
bool clock_set = false;
bool sky_held = false;

}  // namespace

void stage_player(void* local_entity) {
  if (staged || !local_entity) {
    return;
  }
  staged = true;
  // As the script's SetWorldPos: the entity's world matrix (slot 32)
  // with a new translation, set through SetWorldTM (slot 30) for reason 0x400000.
  using game_client::slot;
  const float* current = slot<const float*(__fastcall*)(void*)>(local_entity, 32)(local_entity);
  if (!current) {
    game_client::log("STUDIO player has no world matrix");
    return;
  }
  float matrix[12];
  std::memcpy(matrix, current, sizeof(matrix));
  const float from[3] = {matrix[3], matrix[7], matrix[11]};
  matrix[3] = kStageX;
  matrix[7] = kStageY;
  matrix[11] = kStageZ;
  slot<void(__fastcall*)(void*, const float*, unsigned)>(local_entity, 30)(
      local_entity, matrix, game_client::kScriptSetWorldPosReason);
  game_client::log("STUDIO player staged at (%.1f %.1f %.1f) from (%.1f %.1f %.1f)", kStageX,
                   kStageY, kStageZ, from[0], from[1], from[2]);
}

void keep_light() {
  if (!clock_set) {
    kcd2s::integration::WorldTime noon;
    noon.day = kStudioDay;
    noon.hour = kStudioHour;
    noon.timeScale = 0.0f;
    clock_set = world::apply(noon);
    if (clock_set) {
      game_client::log("STUDIO clock at noon, stopped");
    }
  }
  if (!sky_held) {
    kcd2s::integration::WorldWeather sky;
    sky.preset = kStudioWeather;
    world::hold(sky);
    sky_held = true;
  }
}

void leave() {
  staged = false;
  clock_set = false;
  sky_held = false;
  floor::leave();
}

}  // namespace kcds::runtime::studio
