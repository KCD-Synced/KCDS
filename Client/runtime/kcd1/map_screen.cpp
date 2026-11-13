#include "map_screen.hpp"

#include <Windows.h>

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include "../native_ui/native_ui.hpp"
#include "MinHook.h"
#include "kcd1_map.hpp"
#include "level_files.hpp"
#include "map_labels_movie.hpp"

namespace kcds::runtime::kcd1::map_screen {
namespace {

namespace sdk = kcd2s::sdk;

constexpr std::size_t kRecordSize = 152;
constexpr std::size_t kRecordId = 8;
constexpr std::size_t kRecordLevelName = 16;
constexpr std::size_t kRecordMapName = 48;
constexpr std::size_t kRecordZooms = 56;   // default, smallest, largest
constexpr std::size_t kRecordPlaces = 68;  // seven pairs of floats, see map_screen.hpp
constexpr std::size_t kRecordTiles = 128;
constexpr std::size_t kServicesLevelManager = 200;
constexpr std::size_t kManagerCurrentId = 192;
// The record of KCD1's own Rataje.
constexpr int kRataje = 0;
constexpr char kRatajeName[] = "rataje";
// "mov r8, [rip + records]" at + 5 of the level record lookup, its displacement at + 8.
constexpr std::size_t kRecordsLoad = 5;
constexpr std::size_t kRecordsDisplacement = 8;
constexpr std::size_t kRecordsLoadSize = 7;

// KCD1's map: its four by four tiles of 2048 pixels over the whole 4096 m terrain, opening on
// Rataje.
constexpr float kZooms[3] = {1.0f, -1.0f, 1.0f};
constexpr float kPlaces[14] = {
    4096.0f, 4096.0f,  // the level's size
    0.0f,    0.0f,     // the map's origin in the world
    4096.0f, 4096.0f,  // the map's size in the world
    2533.0f, 547.0f,   // the default place
    4.0f,    4.0f,     // the tile grid
    0.0f,    0.0f,     // the grid's shown part: from
    4.0f,    4.0f,     // and its size
};
constexpr int kTileCount = 16;

// The map labels movie (map_labels_movie.hpp) in the map screen: its clip, file and URL.
constexpr char kElement[] = "ApseMap";
constexpr char kClip[] = "kcdsMapLabels";
constexpr char kMovieKey[] = "_kcds/map-labels-1.swf";
constexpr char kMovieUrl[] = "/%USER%/kcds_ui/_kcds/map-labels-1.swf";
// How often the map screen is looked at, and how long a load may take before it is tried again.
constexpr ULONGLONG kLookMs = 250;
constexpr ULONGLONG kLoadRetryMs = 3000;

using SetLabels = void*(__fastcall*)(void* screen);

Natives natives;
bool shown = false;
SetLabels original_set_labels = nullptr;
// The map screen that last set its labels, and whether they were set while the movie was not in
// it yet (they are set again once it is).
void* labels_screen = nullptr;
bool labels_without_movie = false;
bool movie_written = false;
bool movie_in = false;
ULONGLONG looked_at = 0;
ULONGLONG load_started = 0;

// One NativeUI clip call on the map screen (below its _root).
sdk::Value clip_call(sdk::Value::Array path, const char* action, sdk::Value::Map fields = {}) {
  fields.emplace_back("op", sdk::Value("elementClip"));
  fields.emplace_back("name", sdk::Value(kElement));
  fields.emplace_back("instance", sdk::Value(std::int64_t(0)));
  fields.emplace_back("path", sdk::Value(std::move(path)));
  fields.emplace_back("action", sdk::Value(action));
  return native_ui::request(sdk::Value(std::move(fields)));
}

bool movie_ready() {
  const sdk::Value ready = clip_call({sdk::Value(kClip)}, "get", {{"member", sdk::Value("ready")}});
  return ready.tag() == sdk::Tag::Bool && ready.as_bool();
}

void* __fastcall hooked_set_labels(void* screen) {
  labels_screen = screen;
  labels_without_movie = loaded() && !movie_ready();
  return original_set_labels(screen);
}

template <class T>
T read(const void* object, std::size_t offset) {
  T value;
  std::memcpy(&value, static_cast<const std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

// The level table's records, [first, last).
bool records(std::uint8_t*& first, std::uint8_t*& last) {
  if (!natives.level_record) {
    return false;
  }
  const auto* code = static_cast<const std::uint8_t*>(natives.level_record);
  const auto displacement = read<std::int32_t>(code, kRecordsDisplacement);
  const auto* vector = code + kRecordsLoad + kRecordsLoadSize + displacement;
  first = read<std::uint8_t*>(vector, 0);
  last = read<std::uint8_t*>(vector, sizeof(void*));
  return first && last > first && (last - first) % kRecordSize == 0;
}

std::uint8_t* rataje_record() {
  std::uint8_t* first = nullptr;
  std::uint8_t* last = nullptr;
  if (!records(first, last)) {
    return nullptr;
  }
  for (std::uint8_t* record = first; record < last; record += kRecordSize) {
    if (read<int>(record, kRecordId) != kRataje) {
      continue;
    }
    const char* name = read<const char*>(record, kRecordLevelName);
    if (name && (std::strcmp(name, kRatajeName) == 0 || std::strcmp(name, kLevel) == 0)) {
      return record;
    }
  }
  return nullptr;
}

// The record of KCD1's Rataje as the KCD1 map's, with KCD1's world map.
bool fill(std::uint8_t* record) {
  natives.string_assign(record + kRecordLevelName, kLevel);
  natives.string_assign(record + kRecordMapName, kLevel);
  std::memcpy(record + kRecordZooms, kZooms, sizeof(kZooms));
  std::memcpy(record + kRecordPlaces, kPlaces, sizeof(kPlaces));
  int tiles[kTileCount] = {};
  for (int index = 0; index < kTileCount; ++index) {
    tiles[index] = index + 1;
  }
  const IntVector from{tiles, tiles + kTileCount, tiles + kTileCount};
  natives.assign_ints(record + kRecordTiles, &from);
  return true;
}

}  // namespace

void install(const Natives& given) {
  natives = given;
  if (natives.set_labels &&
      (MH_CreateHook(natives.set_labels, reinterpret_cast<void*>(&hooked_set_labels),
                     reinterpret_cast<void**>(&original_set_labels)) != MH_OK ||
       MH_EnableHook(natives.set_labels) != MH_OK)) {
    original_set_labels = nullptr;
    if (natives.log) {
      natives.log("KCD1 map screen: the map's labels could not be hooked");
    }
  }
}

void tick() {
  const ULONGLONG now = GetTickCount64();
  if (now - looked_at < kLookMs) {
    return;
  }
  looked_at = now;
  if (!native_ui::element_instance(kElement, 0)) {
    labels_screen = nullptr;
    return;
  }
  const bool on_kcd1 = loaded();
  if (movie_ready()) {
    const sdk::Value told =
        clip_call({sdk::Value(kClip)}, "set",
                  {{"member", sdk::Value("hideReputation")}, {"value", sdk::Value(on_kcd1)}});
    if (!movie_in && natives.log) {
      const sdk::Value wrapped =
          clip_call({sdk::Value(kClip)}, "get", {{"member", sdk::Value("wrapped")}});
      natives.log("KCD1 map screen: the labels movie is in (label drawing %s, told %d)",
                  wrapped.tag() == sdk::Tag::Null ? "NOT wrapped" : "wrapped",
                  int(told.tag() == sdk::Tag::Bool && told.as_bool()));
    }
    movie_in = true;
    // Labels the map set before the movie was in it are set again, now without their faces.
    if (labels_without_movie && on_kcd1 && labels_screen && original_set_labels) {
      labels_without_movie = false;
      hooked_set_labels(labels_screen);
      if (natives.log) {
        natives.log("KCD1 map screen: the labels set again without their faces");
      }
    }
    return;
  }
  movie_in = false;
  if (!on_kcd1 || now - load_started < kLoadRetryMs) {
    return;
  }
  if (!movie_written) {
    const std::vector<std::uint8_t> movie = map_labels_movie();
    movie_written = !movie.empty() && native_ui::write_own_file(kMovieKey, movie);
  }
  load_started = now;
  clip_call({}, "create", {{"child", sdk::Value(kClip)}});
  clip_call({sdk::Value(kClip)}, "invoke",
            {{"method", sdk::Value("loadMovie")},
             {"args", sdk::Value(sdk::Value::Array{sdk::Value(kMovieUrl)})}});
  if (natives.log) {
    natives.log("KCD1 map screen: the labels movie loading (written %d)", int(movie_written));
  }
}

void show(const std::string& level, void* services) {
  if (!is_level(level) || !natives.string_assign || !natives.assign_ints) {
    return;
  }
  std::uint8_t* record = rataje_record();
  if (!record) {
    if (natives.log) {
      natives.log("KCD1 map screen: the level table's record of Rataje not found");
    }
    return;
  }
  if (!shown) {
    shown = fill(record);
  }
  void* manager = services ? read<void*>(services, kServicesLevelManager) : nullptr;
  if (manager) {
    std::memcpy(static_cast<std::uint8_t*>(manager) + kManagerCurrentId, &kRataje, sizeof(int));
  }
  if (natives.log) {
    natives.log("KCD1 map screen: KCD1's world map on the level table (level manager %s)",
                manager ? "set" : "NOT found");
  }
}

}  // namespace kcds::runtime::kcd1::map_screen
