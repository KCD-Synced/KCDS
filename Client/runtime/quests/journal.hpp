// The server's quests as rows of the game's own quest menu (the journal), WHGame 1.5.5.
//
// The journal's list (ApseQuestLogList.gfx) is built: the functor makes a
// row (C_UIFlashListQuest, shared, 112 bytes with its count) for every quest of the quest manager
// and the list goes to the movie as "Quests" / "SetQuests". A row is plain data: id (the quest's
// name) and title (texts of its C_UIFlashObject base), E_QuestType, E_QuestProgress, a count,
// the tracked index (-1 untracked), the level id and name, a flag, the location name, and its
// objectives (C_UIFlashListObjective, shared, 64 bytes: id "quest:index", the text, the target
// index, a counter text, two flags). The hook on the functor appends a row for every quest the
// server shows this client, made as the game makes its own (the game's allocator, the classes'
// vtables), so the list frees them as it frees its own.
//
// Selecting a row looks the quest up by name and makes the diary page from it
// (C_UIFlashDiaryQuest, shared, 96 bytes: id, title, description, two lists of
// lines) for "QuestDiary" / "SetQuestDiary". Ours are no quest of the manager: while one of ours is
// selected the diary is made from our quest instead.
//
// The vtables are found by their RTTI names; the allocator (its slot, its first-use check and
// set-up) is read from the diary's maker, as it calls it.
//
// Engine thread.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::quests::journal {

using Logger = void (*)(const char* format, ...);

struct Natives {
  const std::uint8_t* image = nullptr;  // WHGame.dll as loaded
  std::size_t image_size = 0;
  void* list_build = nullptr;        // the journal list's row functor
  void* select = nullptr;            // a row selected
  void* diary_make = nullptr;        // the diary page's maker
  void* focus = nullptr;             // a row of the list focused
  void* track_toggle = nullptr;      // the track button
  void* set_tracked = nullptr;       // "SetQuestTracking" with an index
  void* set_untracked = nullptr;     // "SetQuestTracking" without
  void* vector_insert = nullptr;     // a vector of shared pointers' growing insert
  void* string_from_text = nullptr;  // CryString from a text
  void* string_free = nullptr;
};

// False when a native or a vtable is missing; the journal then shows the game's quests only.
bool install(const Natives& natives, Logger logger);

}  // namespace kcds::runtime::quests::journal
