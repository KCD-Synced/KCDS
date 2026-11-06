// The quests the server writes into the local player's journal and which of them they follow
// (kcd2s/integration/quests.hpp), WHGame 1.5.5.
//
// The client keeps every quest the server shows it, in the order the server sent the orders.
//
//   - A quest shown with `announce` raises the game's quest-updated toast: the HUD's
//     ShowQuestEvent (C_UIHudEvents, its I_UIHudEventsQuest at + 0x58, slot 0) with
//     the quest's key, its progress (E_QuestProgress: 1 Active, 2 Done, 3 Failed), the title as
//     the line to show (not localized) and the journal section (E_QuestType: 0 Main .. 5 Racing).
//     The HUD events are reached as the item transfer is (hud/item_notice.cpp): the game's
//     services + 224, then + 376, which is the C_UIHudEvents' I_UIItemTransfer at + 0x60; the
//     quest interface sits 8 bytes before it. Its slot 0 is checked against the found function.
//   - A followed quest draws every active objective with a position as the game's own quest mark
//     on the compass and the map (markers/blips.hpp, show_objective_point), with the mark type the
//     game gives the quest's section (Main 1, Side 2, Activity, Event and Micro 3,
//     Racing 86). A quest that is done or failed is followed no longer, as the game untracks one
//     it finishes.
//   - Every change of what the player follows, whichever way it happened, is returned to the
//     caller, which reports it to the server (QUEST_TRACKING) and to the resource runtime
//     (questTrackingChanged).
//
// Not yet: the quest is not a row of the game's own journal (apse_questlog) nor of its HUD
// tracker; those list the quest manager's C_Quest nodes.
//
// Engine thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "kcd2s/integration/quests.hpp"

namespace kcds::runtime::quests {

using QuestLogger = void (*)(const char* format, ...);

struct QuestNatives {
  void* (*services)() = nullptr;     // the game services getter
  void* show_quest_event = nullptr;  // C_UIHudEvents::ShowQuestEvent
  void* string_from_text = nullptr;  // CryString from a text
  void* string_free = nullptr;
};

// A change of what the local player follows.
struct TrackingChange {
  std::uint32_t quest = 0;
  std::string key;
  bool tracked = false;
};

// False when a native is missing; quests are then kept and followed, without the toast.
bool install(const QuestNatives& natives, QuestLogger logger);

// Applies one order of the server. What the player follows because of it is appended to
// `changes`.
void apply(const kcd2s::integration::QuestOrder& order, std::vector<TrackingChange>& changes);

// Forgets every quest without touching the registry (a new world is loading; the server sends
// them again when it is ready).
void forget_all();

// How many quests the server shows this client.
std::size_t quest_count();

// A quest the server shows this client, as its journal rows show it (journal.hpp).
struct JournalQuest {
  const kcd2s::integration::QuestShow* quest = nullptr;
  bool tracked = false;
};

// Every quest the server shows this client, in the order of their ids. The pointers hold until
// the next order is applied. Engine thread.
std::vector<JournalQuest> journal_quests();

// The quest of an id, or null.
const JournalQuest* find_journal_quest(std::uint32_t quest, JournalQuest& out);
// The first quest of the journal's, or null when there is none.
const JournalQuest* first_journal_quest(JournalQuest& out);

// The player follows the quest, or no longer does (the journal's track button). False for a quest
// not shown or finished. The change waits for take_player_changes.
bool toggle_by_player(std::uint32_t quest);

// What the player changed in the journal since the last call; reported as the server's orders'
// changes are.
std::vector<TrackingChange> take_player_changes();

}  // namespace kcds::runtime::quests
