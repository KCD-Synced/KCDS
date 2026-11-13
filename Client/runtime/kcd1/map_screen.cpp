#include "map_screen.hpp"

#include <cstdint>
#include <cstring>

#include "kcd1_map.hpp"
#include "level_files.hpp"

namespace kcds::runtime::kcd1::map_screen {
namespace {

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

Natives natives;
bool shown = false;

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
