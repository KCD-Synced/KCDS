// Quest marks on the map screen for our blips and quest objectives of the quest mark types
// WHGame 1.5.5.
//
// The map draws two lists. Points of interest (its marker build, "AddPoiMarkers") take
// their art from the location type, and the quest mark types (0 Checkpoint to 5 ActivityGiver)
// have none there; the quests' marks (C_UIFlashMapQuestMarker, "AddQuestMarkers") are
// built from the list the quest system collects (the tracked quests' objectives and
// their targets), with the game's own Main, Side and other quest art. The hook on that collector
// appends one entry per quest mark of ours (blips.hpp quest_marks), so the map draws them as it
// draws the game's quests; the map adds and removes marks by comparing the list with the one
// before (quest name, objective index and tracked index).
//
// An entry is 56 bytes, as the game fills it: quest name, objective name (CryStrings),
// the journal section's mark type (+16), a quest value (+20, -1), the tracked index (+24), an
// objective id (+32, CryString), the objective's index (+40) and its target (+48), an object
// whose vfunc +16 returns its position and whose +8 is a 64 bit id (the mark's id is "quest:
// index:id"; an id whose top byte is 7, 15, 20 or 21 is an area's). Entries go into the vector
// with the game's copy or growing insert, as the collector's own caller
// does, and the one built here is freed with the game's.
//
// Engine thread (the map's update).
#pragma once

namespace kcds::runtime::markers::map_quest_marks {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* collect = nullptr;           // the quest system's map mark collector
  void* entry_copy = nullptr;        // copy-constructs an entry
  void* entry_insert = nullptr;      // the vector's growing insert
  void* entry_free = nullptr;        // frees an entry's strings
  void* string_from_text = nullptr;  // CryString from a text
  // True when the list holds ours only, without the game's own quests (a multiplayer world).
  bool (*only_ours)() = nullptr;
};

// Hooks the collector; false when a native is missing.
bool install(const Natives& natives, Logger logger);

}  // namespace kcds::runtime::markers::map_quest_marks
