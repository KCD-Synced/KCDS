#include "quests.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "../markers/blips.hpp"

namespace kcds::runtime::quests {
namespace {

namespace wire = kcd2s::integration;

using StringFromText = void*(__fastcall*)(void* string, const char* text);
using StringFree = void(__fastcall*)(void* string);
// I_UIHudEventsQuest slot 0: the quest's key, its E_QuestProgress, the line to show, whether to
// localize that line, and its E_QuestType.
using ShowQuestEvent = void(__fastcall*)(void* events, void* const* key, int progress,
                                         void* const* line, bool localize, int type);

// The game's services: the UI; the UI's item transfer, the HUD events' I_UIItemTransfer.
constexpr std::size_t kServicesUi = 224;
constexpr std::size_t kUiItemTransfer = 376;
// C_UIHudEvents: I_UIHudEventsQuest at + 0x58, I_UIItemTransfer at + 0x60 (constructor).
constexpr std::ptrdiff_t kQuestEventsFromItemTransfer = 0x58 - 0x60;

struct Kept {
  wire::QuestShow quest;
  bool tracked = false;
  // The objectives drawn on the compass and the map, by index.
  std::vector<std::uint32_t> drawn;
};

QuestNatives natives;
QuestLogger report = nullptr;
bool toast_ready = false;
std::unordered_map<std::uint32_t, Kept> kept;
// Track orders for a quest that has not arrived yet: it is followed once it does.
std::unordered_set<std::uint32_t> pending_tracks;
// What the player changed in the journal, not reported yet.
std::vector<TrackingChange> player_changes;

void* field(void* object, std::size_t offset) {
  void* value = nullptr;
  std::memcpy(&value, static_cast<std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

// The HUD's I_UIHudEventsQuest, or null when the HUD is not there or is not laid out as found.
void* quest_events() {
  if (!toast_ready) {
    return nullptr;
  }
  void* services = natives.services();
  void* ui = services ? field(services, kServicesUi) : nullptr;
  auto* transfer = static_cast<std::uint8_t*>(ui ? field(ui, kUiItemTransfer) : nullptr);
  if (!transfer) {
    return nullptr;
  }
  void* events = transfer + kQuestEventsFromItemTransfer;
  void** table = *static_cast<void***>(events);
  if (!table || table[0] != natives.show_quest_event) {
    return nullptr;
  }
  return events;
}

// The game's quest-updated toast for the quest as it now stands.
void toast(const wire::QuestShow& quest) {
  void* events = quest_events();
  if (!events) {
    if (report) {
      report("QUEST toast unavailable for %u", quest.quest);
    }
    return;
  }
  void* key = nullptr;
  void* line = nullptr;
  reinterpret_cast<StringFromText>(natives.string_from_text)(&key, quest.key.c_str());
  reinterpret_cast<StringFromText>(natives.string_from_text)(&line, quest.title.c_str());
  reinterpret_cast<ShowQuestEvent>(natives.show_quest_event)(events, &key, int(quest.progress),
                                                             &line, false, int(quest.type));
  reinterpret_cast<StringFree>(natives.string_free)(&key);
  reinterpret_cast<StringFree>(natives.string_free)(&line);
}

// The mark type the game gives a quest's journal section on the map.
int mark_type_of(wire::QuestType type) {
  switch (type) {
    case wire::QuestType::Main:
      return 1;
    case wire::QuestType::Side:
      return 2;
    case wire::QuestType::Racing:
      return 86;
    case wire::QuestType::Activity:
    case wire::QuestType::Event:
    case wire::QuestType::Micro:
      return 3;
  }
  return 2;
}

bool finished(const wire::QuestShow& quest) {
  return quest.progress == wire::QuestProgress::Done ||
         quest.progress == wire::QuestProgress::Failed;
}

void erase_points(Kept& entry) {
  for (const std::uint32_t index : entry.drawn) {
    markers::hide_objective_point(entry.quest.quest, index);
  }
  entry.drawn.clear();
}

// A followed, active quest shows its active objectives that have a position.
void draw_points(Kept& entry) {
  erase_points(entry);
  if (!entry.tracked || entry.quest.progress != wire::QuestProgress::Active) {
    return;
  }
  const int type = mark_type_of(entry.quest.type);
  for (std::uint32_t index = 0; index < entry.quest.objectives.size(); ++index) {
    const auto& objective = entry.quest.objectives[index];
    if (!objective.has_position || objective.progress != wire::QuestProgress::Active) {
      continue;
    }
    if (markers::show_objective_point(entry.quest.quest, index, objective.position, type)) {
      entry.drawn.push_back(index);
    }
  }
}

// Follows the quest or stops; a change goes to `changes`.
void set_tracked(Kept& entry, bool tracked, std::vector<TrackingChange>& changes) {
  const bool changed = entry.tracked != tracked;
  entry.tracked = tracked;
  draw_points(entry);
  if (!changed) {
    return;
  }
  changes.push_back(TrackingChange{entry.quest.quest, entry.quest.key, tracked});
  if (report) {
    report("QUEST %s %u %s, %zu objective marks", tracked ? "followed" : "no longer followed",
           entry.quest.quest, entry.quest.key.c_str(), entry.drawn.size());
  }
}

void show(const wire::QuestShow& quest, std::vector<TrackingChange>& changes) {
  Kept& entry = kept[quest.quest];
  const bool fresh = entry.quest.quest == 0;
  entry.quest = quest;
  if (quest.announce) {
    toast(quest);
  }
  if (report) {
    report("QUEST %s %u %s type=%s progress=%s objectives=%zu announce=%d",
           fresh ? "shown" : "updated", quest.quest, quest.key.c_str(),
           wire::quest_type_name(quest.type), wire::quest_progress_name(quest.progress),
           quest.objectives.size(), int(quest.announce));
  }
  bool tracked = entry.tracked;
  if (pending_tracks.erase(quest.quest) > 0) {
    tracked = true;
  }
  // The game stops following a quest it finished or failed.
  if (finished(quest)) {
    tracked = false;
  }
  set_tracked(entry, tracked, changes);
}

void hide(std::uint32_t quest) {
  pending_tracks.erase(quest);
  const auto found = kept.find(quest);
  if (found == kept.end()) {
    return;
  }
  erase_points(found->second);
  if (report) {
    report("QUEST removed %u %s", quest, found->second.quest.key.c_str());
  }
  kept.erase(found);
}

void track(std::uint32_t quest, bool tracked, std::vector<TrackingChange>& changes) {
  const auto found = kept.find(quest);
  if (found == kept.end()) {
    if (tracked) {
      pending_tracks.insert(quest);
    } else {
      pending_tracks.erase(quest);
    }
    return;
  }
  if (tracked && finished(found->second.quest)) {
    return;
  }
  set_tracked(found->second, tracked, changes);
}

}  // namespace

bool install(const QuestNatives& found, QuestLogger logger) {
  natives = found;
  report = logger;
  toast_ready = natives.services && natives.show_quest_event && natives.string_from_text &&
                natives.string_free;
  return toast_ready;
}

void apply(const wire::QuestOrder& order, std::vector<TrackingChange>& changes) {
  switch (order.kind) {
    case wire::QuestOrder::Kind::Show:
      show(order.show, changes);
      break;
    case wire::QuestOrder::Kind::Hide:
      hide(order.show.quest);
      break;
    case wire::QuestOrder::Kind::Track:
      track(order.show.quest, order.tracked, changes);
      break;
  }
}

void forget_all() {
  kept.clear();
  pending_tracks.clear();
  markers::forget_objective_points();
}

std::size_t quest_count() {
  return kept.size();
}

std::vector<JournalQuest> journal_quests() {
  std::vector<std::uint32_t> ids;
  ids.reserve(kept.size());
  for (const auto& [id, entry] : kept) {
    ids.push_back(id);
  }
  std::sort(ids.begin(), ids.end());
  std::vector<JournalQuest> quests;
  quests.reserve(ids.size());
  for (const std::uint32_t id : ids) {
    const Kept& entry = kept.at(id);
    quests.push_back(JournalQuest{&entry.quest, entry.tracked});
  }
  return quests;
}

bool toggle_by_player(std::uint32_t quest) {
  const auto found = kept.find(quest);
  if (found == kept.end()) {
    return false;
  }
  const bool tracked = !found->second.tracked;
  if (tracked && finished(found->second.quest)) {
    return false;
  }
  set_tracked(found->second, tracked, player_changes);
  return true;
}

std::vector<TrackingChange> take_player_changes() {
  return std::exchange(player_changes, {});
}

const JournalQuest* first_journal_quest(JournalQuest& out) {
  if (kept.empty()) {
    return nullptr;
  }
  out = JournalQuest{&kept.begin()->second.quest, kept.begin()->second.tracked};
  return &out;
}

const JournalQuest* find_journal_quest(std::uint32_t quest, JournalQuest& out) {
  const auto found = kept.find(quest);
  if (found == kept.end()) {
    return nullptr;
  }
  out = JournalQuest{&found->second.quest, found->second.tracked};
  return &out;
}

}  // namespace kcds::runtime::quests
