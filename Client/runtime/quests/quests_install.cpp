// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "quests_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../markers/map_quest_marks.hpp"
#include "journal.hpp"
#include "quests.hpp"

namespace kcds::game_client {

void install_quests() {
  // Quests: the HUD's quest-updated toast; the objectives of a followed quest
  // are drawn as points of the same registry as the blips.
  kcds::runtime::quests::QuestNatives quest_natives;
  quest_natives.services = get_services;
  quest_natives.show_quest_event =
      resolve_unique("ShowQuestEvent",
                     "48 89 5C 24 10 48 89 74 24 20 55 57 41 56 48 8B EC 48 83 EC 60 F6 05 "
                     "?? ?? ?? ?? 10 41 8D 40 FF 89 45 20 48 8B F2 4C 8B F1 0F 85");
  quest_natives.string_from_text =
      resolve_unique("CryStringFromText",
                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 44 8B 05 ?? ?? "
                     "?? 04 48 8B F1 65 48 8B 04 25 58 00 00 00 48 83 CB FF B9 18 00 00 00");
  quest_natives.string_free =
      resolve_unique("CryStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8 ?? ?? ?? ??");
  log("QUESTS toast %s",
      kcds::runtime::quests::install(quest_natives, log) ? "installed" : "unavailable");
  // The server's quests as rows of the game's quest menu.
  kcds::runtime::quests::journal::Natives journal_natives;
  journal_natives.image = image;
  journal_natives.image_size = image_size;
  journal_natives.list_build = resolve_unique(
      "QuestJournalRows",
      "48 89 5C 24 10 48 89 6C 24 18 48 89 7C 24 20 41 56 48 83 EC 40 48 8B 7A 08 4D 8B F0 "
      "48 8B 1A 48 8B E9 48 3B DF 74 70");
  // A row selected; the diary page's maker from its own call (+ 0x39).
  constexpr const char* kQuestSelect =
      "48 89 5C 24 18 55 56 57 48 8B EC 48 83 EC 40 48 8B F2 48 8B F9 E8 ?? ?? ?? ?? 4C 8B C6 "
      "48 8D 55 28 48 8B 88 18 01 00 00 48 8B 49 10 E8 ?? ?? ?? ?? 48 8B 55 28 48 8D 4D E0 E8";
  journal_natives.select = resolve_unique("QuestJournalSelect", kQuestSelect);
  journal_natives.diary_make = resolve_unique("QuestJournalDiary", kQuestSelect, 0x3A, 0x3E);
  journal_natives.track_toggle = resolve_unique(
      "QuestJournalTrack",
      "40 55 53 56 57 41 56 41 57 48 8D 6C 24 D1 48 81 EC 88 00 00 00 4C 8B F1 48 8D 4D 6F E8 "
      "?? ?? ?? ?? 48 8D 4D D7 E8 ?? ?? ?? ?? 49 8D BE C0 00 00");
  // SetQuestTracking with the tracked index, from the track button's own call (+ 0x2B).
  journal_natives.set_tracked = resolve_unique(
      "QuestJournalTracked",
      "48 8B D3 49 8B CE E8 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8D 4D 7F E8 ?? ?? ?? ?? 4C 8D "
      "4D 67 48 8B CF 4C 8D 45 6F 48 8D 55 7F E8",
      43, 47);
  journal_natives.set_untracked = resolve_unique(
      "QuestJournalUntracked",
      "48 89 5C 24 08 48 89 74 24 10 4C 89 4C 24 20 57 48 83 EC 60 48 8B 01 49 8B F8 48 8B F2 "
      "48 8B D9 FF 50 60 48 8B 5B 48 48 85 DB 75 04 32 C0 EB 5C");
  journal_natives.string_free = quest_natives.string_free;
  journal_natives.focus = resolve_unique(
      "QuestJournalFocus",
      "48 8B C4 48 89 58 10 48 89 70 20 44 88 40 18 55 57 41 54 41 56 41 57 48 8D 68 A1 48 81 "
      "EC C0 00 00 00 4C 8B F1 45 8A F9 48 8D 4D CF 48 8B DA E8");
  // The growing insert of a vector of shared pointers, from the quest marks' update (+ 0x2B).
  journal_natives.vector_insert = resolve_unique(
      "SharedVectorInsert",
      "0F 28 45 27 66 0F 7F 45 97 48 3B 4D B7 74 10 48 8D 55 97 E8 ?? ?? ?? ?? 48 83 45 AF 10 "
      "EB 10 48 8B D1 4C 8D 45 97 48 8D 4D A7 E8",
      43, 47);
  journal_natives.string_from_text = quest_natives.string_from_text;
  log("QUESTS journal %s",
      kcds::runtime::quests::journal::install(journal_natives, log) ? "installed" : "unavailable");
  // Our blips and objectives of the quest mark types as quest marks on the map.
  // The entry copy and the growing insert are taken from the collector's caller's own calls
  // (+ 0x51 and + 0x66).
  constexpr const char* kQuestMarkFilter =
      "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B 6A 08 49 8B D8 48 "
      "8B 3A 48 8B F1 4C 89 4C 24 30 48 3B FD 74 4E 0F 1F 44 00 00 48 8B D7 48 8D 4C 24 30 "
      "E8 ?? ?? ?? ?? 84 C0 74 2A 48 8B 43 08 48 3B 43 10 74 12 48 8B D7 48 8B C8 E8 ?? ?? "
      "?? ?? 48 83 43 08 38 EB 0E 4C 8B C7 48 8B D0 48 8B CB E8";
  kcds::runtime::markers::map_quest_marks::Natives quest_mark_natives;
  quest_mark_natives.collect = resolve_unique(
      "QuestMapMarkCollect",
      "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 50 48 8B F2 48 C7 02 00 00 00 00 48 8B D9 "
      "48 C7 42 08 00 00 00 00 48 C7 42 10 00 00 00 00 E8");
  quest_mark_natives.entry_copy = resolve_unique("QuestMapMarkCopy", kQuestMarkFilter, 0x52, 0x56);
  quest_mark_natives.entry_insert =
      resolve_unique("QuestMapMarkInsert", kQuestMarkFilter, 0x67, 0x6B);
  quest_mark_natives.entry_free =
      resolve_unique("QuestMapMarkFree",
                     "40 53 48 83 EC 20 48 8B D9 48 8B 49 20 48 83 E9 0C E8 ?? ?? ?? ?? 48 8B "
                     "4B 08 48 83 E9 0C E8 ?? ?? ?? ?? 48 8B 0B 48 83 E9 0C 48 83 C4 20 5B E9");
  quest_mark_natives.string_from_text = quest_natives.string_from_text;
  // In a multiplayer world the map's quest marks are ours only: the game's own quests are not
  // the server's.
  quest_mark_natives.only_ours = [] { return multiplayer_world.load(); };
  log("MARKERS quest marks on the map %s",
      kcds::runtime::markers::map_quest_marks::install(quest_mark_natives, log) ? "installed"
                                                                                : "unavailable");
}

}  // namespace kcds::game_client
