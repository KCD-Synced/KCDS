#include "journal.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <string>

#include "MinHook.h"
#include "kcd2s/bridge/sigscan.hpp"
#include "quests.hpp"

namespace kcds::runtime::quests::journal {
namespace {

namespace wire = kcd2s::integration;

// What every row of ours is called: the prefix, then the quest's id.
constexpr char kIdPrefix[] = "kcds_quest_";
// The level a row of ours lists under: none (the game's "other levels").
constexpr std::int32_t kNoLevel = -1;
constexpr char kOtherLevels[] = "ui_other_levels";

// A shared object as std::make_shared lays it out: the count block, then the object.
constexpr std::size_t kCountBlock = 16;
constexpr std::size_t kListQuestSize = 96;
constexpr std::size_t kListObjectiveSize = 48;
constexpr std::size_t kDiarySize = 80;

// C_UIFlashListQuest
constexpr std::size_t kId = 8;
constexpr std::size_t kTitle = 16;
constexpr std::size_t kQuestType = 24;
constexpr std::size_t kQuestProgress = 28;
constexpr std::size_t kQuestCount = 32;
constexpr std::size_t kQuestTracked = 36;
constexpr std::size_t kQuestLevel = 40;
constexpr std::size_t kQuestLevelName = 48;
constexpr std::size_t kQuestFlag = 56;
constexpr std::size_t kQuestLocation = 64;
constexpr std::size_t kQuestObjectives = 72;
// C_UIFlashListObjective
constexpr std::size_t kObjectiveTarget = 24;
constexpr std::size_t kObjectiveCounter = 32;
constexpr std::size_t kObjectiveOptional = 40;
constexpr std::size_t kObjectiveArea = 41;
// C_UIFlashDiaryQuest
constexpr std::size_t kDiaryDescription = 24;

// The diary's maker calls the allocator as every maker of a shared UI object does:
// `cmp [first use], 0` at + 0x0F, `call set up` at + 0x1E, `mov rax, [allocator]` at + 0x23.
constexpr std::size_t kAllocFirstUse = 0x0F;
constexpr std::size_t kAllocSetUp = 0x1E;
constexpr std::size_t kAllocSlot = 0x23;

struct SharedVector {
  std::uint8_t* first;
  std::uint8_t* last;
  std::uint8_t* end;
};

struct SharedRef {
  void* object;
  std::uint8_t* block;
};

using ListBuild = void*(__fastcall*)(void* result, void* quests, void* context);
using Select = std::intptr_t(__fastcall*)(void* menu, void* name);
using DiaryMake = SharedRef*(__fastcall*)(SharedRef* out, void* quest);
using VectorInsert = void(__fastcall*)(SharedVector* vector, std::uint8_t* where, SharedRef* value);
using StringFromText = void*(__fastcall*)(void* string, const char* text);
using Allocate = void*(__fastcall*)(std::uint32_t size, std::uint32_t* allocated, int flags);
using SetUp = void(__fastcall*)();

Natives natives;
Logger log_line = nullptr;
ListBuild original_list_build = nullptr;
Select original_select = nullptr;
DiaryMake original_diary_make = nullptr;
const std::int32_t* alloc_first_use = nullptr;
SetUp alloc_set_up = nullptr;
Allocate* alloc_slot = nullptr;

struct Vtables {
  const void* list_quest = nullptr;
  const void* list_quest_count = nullptr;
  const void* list_objective = nullptr;
  const void* list_objective_count = nullptr;
  const void* diary = nullptr;
  const void* diary_count = nullptr;
};
Vtables vtables;

// The quest the journal has selected while it makes the diary page (engine thread).
std::string selected;

// The target of a rip-relative operand whose 32 bits are at `at` and whose instruction ends at
// `end`.
const std::uint8_t* rip_target(const std::uint8_t* at, const std::uint8_t* end) {
  std::int32_t displacement = 0;
  std::memcpy(&displacement, at, sizeof(displacement));
  return end + displacement;
}

// The vtable of the class whose RTTI type name is `name` (kcd2s::bridge::rtti_vtable).
const void* rtti_vtable(const char* name) {
  const std::size_t at = kcd2s::bridge::rtti_vtable(
      natives.image, natives.image_size, reinterpret_cast<std::uintptr_t>(natives.image), name);
  return at == kcd2s::bridge::knpos ? nullptr : natives.image + at;
}

void* allocate(std::uint32_t size) {
  if (*alloc_first_use == 0) {
    alloc_set_up();
  }
  std::uint32_t allocated = 0;
  return (*alloc_slot)(size, &allocated, 0);
}

void put_text(std::uint8_t* object, std::size_t offset, const std::string& text) {
  reinterpret_cast<StringFromText>(natives.string_from_text)(object + offset, text.c_str());
}

template <class T>
void put(std::uint8_t* object, std::size_t offset, T value) {
  std::memcpy(object + offset, &value, sizeof(value));
}

// A shared object of the class `vtable`, `size` bytes, its counts 1: as the game's makers make
// them. The object's own fields are zero.
SharedRef make_shared(const void* count_vtable, const void* vtable, std::size_t size) {
  auto* block = static_cast<std::uint8_t*>(allocate(std::uint32_t(kCountBlock + size)));
  if (!block) {
    return {nullptr, nullptr};
  }
  std::memset(block, 0, kCountBlock + size);
  put(block, 0, count_vtable);
  put(block, 8, std::uint32_t(1));
  put(block, 12, std::uint32_t(1));
  std::uint8_t* object = block + kCountBlock;
  put(object, 0, vtable);
  return {object, block};
}

// Appends `value` to the vector, which takes it over: the insert moves it in (it left the
// pointer it was given empty).
void push_shared(SharedVector* vector, SharedRef value) {
  reinterpret_cast<VectorInsert>(natives.vector_insert)(vector, vector->last, &value);
}

std::string row_id(std::uint32_t quest) {
  return std::string(kIdPrefix) + std::to_string(quest);
}

SharedRef make_objective(std::uint32_t quest, std::size_t index, const wire::QuestObjective& o) {
  SharedRef row =
      make_shared(vtables.list_objective_count, vtables.list_objective, kListObjectiveSize);
  if (!row.object) {
    return row;
  }
  auto* object = static_cast<std::uint8_t*>(row.object);
  put_text(object, kId, row_id(quest) + ":" + std::to_string(index));
  put_text(object, kTitle, o.text);
  put(object, kObjectiveTarget, std::int32_t(o.has_position ? std::int32_t(index) : -1));
  put_text(object, kObjectiveCounter, "");
  put(object, kObjectiveOptional, std::uint8_t(o.optional ? 1 : 0));
  put(object, kObjectiveArea, std::uint8_t(0));
  return row;
}

SharedRef make_row(const JournalQuest& entry) {
  const wire::QuestShow& quest = *entry.quest;
  SharedRef row = make_shared(vtables.list_quest_count, vtables.list_quest, kListQuestSize);
  if (!row.object) {
    return row;
  }
  auto* object = static_cast<std::uint8_t*>(row.object);
  put_text(object, kId, row_id(quest.quest));
  put_text(object, kTitle, quest.title);
  put(object, kQuestType, std::int32_t(quest.type));
  put(object, kQuestProgress, std::int32_t(quest.progress));
  put(object, kQuestCount, std::int32_t(0));
  put(object, kQuestTracked, std::int32_t(entry.tracked ? 0 : -1));
  put(object, kQuestLevel, kNoLevel);
  put_text(object, kQuestLevelName, kOtherLevels);
  put(object, kQuestFlag, std::uint8_t(0));
  put_text(object, kQuestLocation, "");
  auto* objectives = reinterpret_cast<SharedVector*>(object + kQuestObjectives);
  for (std::size_t index = 0; index < quest.objectives.size(); ++index) {
    const wire::QuestObjective& objective = quest.objectives[index];
    if (objective.progress != wire::QuestProgress::Active &&
        objective.progress != wire::QuestProgress::Done) {
      continue;
    }
    const SharedRef child = make_objective(quest.quest, index, objective);
    if (child.object) {
      push_shared(objectives, child);
    }
  }
  return row;
}

void* __fastcall hooked_list_build(void* result, void* quests, void* context) {
  void* built = original_list_build(result, quests, context);
  auto* rows = context ? static_cast<SharedVector**>(context)[1] : nullptr;
  if (!rows) {
    return built;
  }
  std::size_t added = 0;
  for (const JournalQuest& entry : journal_quests()) {
    const SharedRef row = make_row(entry);
    if (row.object) {
      push_shared(rows, row);
      ++added;
    }
  }
  static std::size_t logged = SIZE_MAX;
  if (log_line && added != logged) {
    logged = added;
    log_line("QUESTS journal rows of ours: %zu", added);
  }
  return built;
}

// Our quest the journal has focused (its id), 0 for a quest of the game's or none.
std::uint32_t focused = 0;

std::intptr_t __fastcall hooked_select(void* menu, void* name) {
  const char* text = name ? *static_cast<const char* const*>(name) : nullptr;
  selected = text ? text : "";
  const std::intptr_t result = original_select(menu, name);
  selected.clear();
  return result;
}

// Our quest of the selected row, or null.
const JournalQuest* selected_quest(JournalQuest& out) {
  const std::size_t prefix = sizeof(kIdPrefix) - 1;
  if (selected.compare(0, prefix, kIdPrefix) != 0) {
    return nullptr;
  }
  const unsigned long id = std::strtoul(selected.c_str() + prefix, nullptr, 10);
  return find_journal_quest(std::uint32_t(id), out);
}

// The texts the game's diary localizes of a quest (calls with + 248 and
// + 264; it reads the CryString at + 8 of each, and its length 8 bytes before the characters):
// readable memory, or a quest the diary must not be made of.
constexpr std::size_t kQuestTitleText = 248 + 8;
constexpr std::size_t kQuestDescriptionText = 264 + 8;
constexpr std::size_t kCryStringHeader = 8;

bool readable(const void* address, std::size_t size) {
  MEMORY_BASIC_INFORMATION info{};
  if (!address || VirtualQuery(address, &info, sizeof(info)) != sizeof(info) ||
      info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
    return false;
  }
  const auto* end = static_cast<const std::uint8_t*>(info.BaseAddress) + info.RegionSize;
  return static_cast<const std::uint8_t*>(address) + size <= end;
}

bool diary_texts_readable(void* quest) {
  for (const std::size_t offset : {kQuestTitleText, kQuestDescriptionText}) {
    const auto* slot = static_cast<const std::uint8_t*>(quest) + offset;
    if (!readable(slot, sizeof(void*))) {
      return false;
    }
    const auto* text = *reinterpret_cast<const std::uint8_t* const*>(slot);
    if (!text || !readable(text - kCryStringHeader, kCryStringHeader + 1)) {
      return false;
    }
  }
  return true;
}

SharedRef* __fastcall hooked_diary_make(SharedRef* out, void* quest) {
  JournalQuest entry;
  const JournalQuest* ours = selected_quest(entry);
  const bool by_selection = ours != nullptr;
  if (!ours && quest && diary_texts_readable(quest)) {
    return original_diary_make(out, quest);
  }
  // No quest of the game's for the name (a double click selects "kcds_quest_0":
  // the game's diary then read texts at 0 + 248 and crashed, and an empty one crashed later): the
  // diary of our focused quest, else of our first one.
  if (!ours && focused != 0) {
    ours = find_journal_quest(focused, entry);
  }
  if (!ours) {
    ours = first_journal_quest(entry);
  }
  if (!ours) {
    return original_diary_make(out, quest);
  }
  if (log_line && !by_selection) {
    log_line("QUESTS journal: \"%s\" is none of ours; the diary of quest %u", selected.c_str(),
             ours->quest->quest);
  }
  SharedRef diary = make_shared(vtables.diary_count, vtables.diary, kDiarySize);
  if (diary.object) {
    auto* object = static_cast<std::uint8_t*>(diary.object);
    put_text(object, kId, row_id(ours->quest->quest));
    put_text(object, kTitle, ours->quest->title);
    put_text(object, kDiaryDescription, ours->quest->description);
  }
  *out = diary;
  return out;
}

// A row of the list focused (menu, the row's ids, two flags). For a quest of the
// manager it shows the diary page and asks the quest again for the track and map buttons; ours is
// none (the lookup's empty answer crashed). For a row of ours, a quest or one of its
// objectives ("<quest id>:<index>"), only the diary page is shown; the buttons stay as they are.
using Focus = std::intptr_t(__fastcall*)(void* menu, void* id, char first, char second);
Focus original_focus = nullptr;

std::intptr_t __fastcall hooked_focus(void* menu, void* id, char first, char second) {
  // `id` is a vector of CryStrings; the first is the row's id.
  const auto* ids = static_cast<const char* const* const*>(id);
  const char* text = ids && ids[0] != ids[1] ? *ids[0] : nullptr;
  const std::size_t prefix = sizeof(kIdPrefix) - 1;
  if (!text || std::strncmp(text, kIdPrefix, prefix) != 0) {
    focused = 0;
    return original_focus(menu, id, first, second);
  }
  std::string quest = text;
  if (const std::size_t colon = quest.find(':'); colon != std::string::npos) {
    quest.resize(colon);
  }
  // A double click focuses "kcds_quest_0", none of ours: the quest stays the one
  // focused before, else our first, so the track button never hands the game a quest of ours.
  const auto number = std::uint32_t(std::strtoul(quest.c_str() + prefix, nullptr, 10));
  JournalQuest entry;
  if (find_journal_quest(number, entry)) {
    focused = number;
  } else if (focused == 0 || !find_journal_quest(focused, entry)) {
    focused = first_journal_quest(entry) ? entry.quest->quest : 0;
  }
  if (focused == 0) {
    return original_focus(menu, id, first, second);
  }
  quest = row_id(focused);
  // The selection takes over the name it is given (it frees it).
  void* name = nullptr;
  reinterpret_cast<StringFromText>(natives.string_from_text)(&name, quest.c_str());
  hooked_select(menu, &name);
  return 0;
}

// The track button (the menu): for the focused quest of the manager it reads the
// quest's type for the track limit; ours is none. For ours the player follows it, or no longer
// does (quests.hpp toggle_by_player, reported to the server as any change), and the row is told as
// the game tells its own: "SetQuestTracking" with the row's id and the tracked index,
// or without it to untrack, on the menu's movie (+ 192).
using TrackToggle = std::intptr_t(__fastcall*)(void* menu);
using SetTracked = bool(__fastcall*)(void* movie, void* const* function, void* const* id,
                                     const std::int32_t* index);
using SetUntracked = bool(__fastcall*)(void* movie, void* const* function, void* const* id);
using StringFree = void(__fastcall*)(void* string);
TrackToggle original_track_toggle = nullptr;
constexpr std::size_t kMenuMovie = 192;

std::intptr_t __fastcall hooked_track_toggle(void* menu) {
  if (focused == 0) {
    return original_track_toggle(menu);
  }
  if (!toggle_by_player(focused)) {
    return 0;
  }
  JournalQuest entry;
  if (!find_journal_quest(focused, entry)) {
    return 0;
  }
  const auto from_text = reinterpret_cast<StringFromText>(natives.string_from_text);
  const auto free_string = reinterpret_cast<StringFree>(natives.string_free);
  void* function = nullptr;
  void* id = nullptr;
  from_text(&function, "SetQuestTracking");
  from_text(&id, row_id(focused).c_str());
  void* movie = static_cast<std::uint8_t*>(menu) + kMenuMovie;
  if (entry.tracked) {
    const std::int32_t index = 0;
    reinterpret_cast<SetTracked>(natives.set_tracked)(movie, &function, &id, &index);
  } else {
    reinterpret_cast<SetUntracked>(natives.set_untracked)(movie, &function, &id);
  }
  free_string(&id);
  free_string(&function);
  return 0;
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.image || !natives.list_build || !natives.select || !natives.diary_make ||
      !natives.focus || !natives.track_toggle || !natives.set_tracked || !natives.set_untracked ||
      !natives.vector_insert || !natives.string_from_text || !natives.string_free) {
    return false;
  }
  const auto* maker = static_cast<const std::uint8_t*>(natives.diary_make);
  if (maker[kAllocFirstUse] != 0x83 || maker[kAllocFirstUse + 1] != 0x3D ||
      maker[kAllocSetUp] != 0xE8 || maker[kAllocSlot] != 0x48 || maker[kAllocSlot + 1] != 0x8B ||
      maker[kAllocSlot + 2] != 0x05) {
    return false;
  }
  alloc_first_use = reinterpret_cast<const std::int32_t*>(
      rip_target(maker + kAllocFirstUse + 2, maker + kAllocFirstUse + 7));
  alloc_set_up = reinterpret_cast<SetUp>(
      const_cast<std::uint8_t*>(rip_target(maker + kAllocSetUp + 1, maker + kAllocSetUp + 5)));
  alloc_slot = reinterpret_cast<Allocate*>(
      const_cast<std::uint8_t*>(rip_target(maker + kAllocSlot + 3, maker + kAllocSlot + 7)));
  vtables.list_quest = rtti_vtable(".?AVC_UIFlashListQuest@guimodule@wh@@");
  vtables.list_quest_count =
      rtti_vtable(".?AV?$_Ref_count_obj2@VC_UIFlashListQuest@guimodule@wh@@@std@@");
  vtables.list_objective = rtti_vtable(".?AVC_UIFlashListObjective@guimodule@wh@@");
  vtables.list_objective_count =
      rtti_vtable(".?AV?$_Ref_count_obj2@VC_UIFlashListObjective@guimodule@wh@@@std@@");
  vtables.diary = rtti_vtable(".?AVC_UIFlashDiaryQuest@guimodule@wh@@");
  vtables.diary_count =
      rtti_vtable(".?AV?$_Ref_count_obj2@VC_UIFlashDiaryQuest@guimodule@wh@@@std@@");
  if (log_line) {
    log_line("QUESTS journal vtables %p %p %p %p %p %p", vtables.list_quest,
             vtables.list_quest_count, vtables.list_objective, vtables.list_objective_count,
             vtables.diary, vtables.diary_count);
  }
  if (!vtables.list_quest || !vtables.list_quest_count || !vtables.list_objective ||
      !vtables.list_objective_count || !vtables.diary || !vtables.diary_count) {
    return false;
  }
  return hook(natives.list_build, reinterpret_cast<void*>(&hooked_list_build),
              reinterpret_cast<void**>(&original_list_build)) &&
         hook(natives.select, reinterpret_cast<void*>(&hooked_select),
              reinterpret_cast<void**>(&original_select)) &&
         hook(natives.diary_make, reinterpret_cast<void*>(&hooked_diary_make),
              reinterpret_cast<void**>(&original_diary_make)) &&
         hook(natives.focus, reinterpret_cast<void*>(&hooked_focus),
              reinterpret_cast<void**>(&original_focus)) &&
         hook(natives.track_toggle, reinterpret_cast<void*>(&hooked_track_toggle),
              reinterpret_cast<void**>(&original_track_toggle));
}

}  // namespace kcds::runtime::quests::journal
