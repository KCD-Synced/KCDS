// The game's map screen (M) on the KCD1 map: KCD1's own world map (its tiles in the assets pack,
// level_files.hpp map_tile_name).
//
// The map screen shows the current level's record of the game's level table
// (Libs/Tables/level.xml, C_LevelData records 152 bytes apart, sorted by their id at + 8; the
// level manager, the game services' + 200, keeps the current level's id at + 192 and finds a
// level's record by its name at + 16). The table has no record of the KCD1 map, so the screen
// had none and kept KCD2's last map. Its record of KCD1's own Rataje (id 0, "rataje", no map),
// which no KCD2 level uses, becomes the KCD1 map's: its name, and its map (+ 48 the map's name,
// + 56 default, smallest and largest zoom, + 68 the level's size, + 76 the map's origin in the
// world, + 84 its size in the world, + 92 the default place, + 100 the tile grid, + 108 and
// + 116 the grid's shown part, + 128 the tiles there are, a vector of ints).
//
// Engine thread.
#pragma once

#include <string>

namespace kcds::runtime::kcd1::map_screen {

using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void* text, const char* value);

struct IntVector {
  int* first = nullptr;
  int* last = nullptr;
  int* end = nullptr;
};
// std::vector<int>'s assignment as the game has it (its own allocator).
using AssignInts = void*(__fastcall*)(void* vector, const IntVector* from);

struct Natives {
  // The level manager's record of a level by id: its first instructions read the table's records.
  void* level_record = nullptr;
  StringAssign string_assign = nullptr;
  AssignInts assign_ints = nullptr;
  // The map screen's setting of its labels (SetMapLabels, the screen as its argument).
  void* set_labels = nullptr;
  Logger log = nullptr;
};

void install(const Natives& natives);

// When a world on the KCD1 map has loaded (nothing for another level): the table's record of
// KCD1's Rataje becomes the KCD1 map's, and the level manager's current level.
void show(const std::string& level, void* services);

// Every frame: while the map screen is open on the KCD1 map, its labels show no reputation face
// (KCD1's places have none of KCD2's factions, so the game showed the worst for each): the labels
// movie (map_labels_movie.hpp) is loaded into the screen and told to hide them.
void tick();

}  // namespace kcds::runtime::kcd1::map_screen
