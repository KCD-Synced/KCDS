#include "map_quest_marks.hpp"

#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "blips.hpp"

namespace kcds::runtime::markers::map_quest_marks {
namespace {

// The quest system's map mark entry (fills it).
struct Entry {
  const char* quest = nullptr;      // CryString
  const char* objective = nullptr;  // CryString
  std::int32_t type = 0;
  std::int32_t value = -1;
  std::int32_t tracked = 0;
  std::int32_t unused = 0;
  const char* id = nullptr;  // CryString
  std::int32_t objective_index = 0;
  std::int32_t unused2 = 0;
  void* target = nullptr;
};
static_assert(sizeof(Entry) == 56);

// The game's vector of entries.
struct EntryVector {
  Entry* first;
  Entry* last;
  Entry* end;
};

// A mark's target: the map reads its position through vfunc +16 and its id at +8. It lives as
// long as the session: the map keeps the entries of the list before (and reads the id of a mark it
// removes through the target), so a target is never freed.
struct Target {
  void* const* vtable;
  std::uint64_t id;
  float position[3];
};

const float* __fastcall target_position(const Target* self) {
  return self->position;
}

void* __fastcall target_nothing(const Target*) {
  return nullptr;
}

void* const kTargetVtable[8] = {
    reinterpret_cast<void*>(&target_nothing),  reinterpret_cast<void*>(&target_nothing),
    reinterpret_cast<void*>(&target_position), reinterpret_cast<void*>(&target_nothing),
    reinterpret_cast<void*>(&target_nothing),  reinterpret_cast<void*>(&target_nothing),
    reinterpret_cast<void*>(&target_nothing),  reinterpret_cast<void*>(&target_nothing),
};

// The id's top byte is ours (0x4B), never one the map takes for an area's (7, 15, 20, 21).
constexpr std::uint64_t kTargetIdTag = 0x4Bull << 56;
constexpr std::uint64_t kTargetIdMask = (1ull << 56) - 1;

using Collect = EntryVector*(__fastcall*)(void* quests, EntryVector* out);
using EntryCopy = void(__fastcall*)(Entry* at, const Entry* from);
using EntryInsert = void(__fastcall*)(EntryVector* vector, Entry* where, const Entry* from);
using EntryFree = void(__fastcall*)(Entry* entry);
using StringFromText = void*(__fastcall*)(void* string, const char* text);

Natives natives;
Logger log_line = nullptr;
Collect original_collect = nullptr;
std::unordered_map<std::uint64_t, std::unique_ptr<Target>> targets;
std::vector<QuestMark> marks;

Target* target_of(const QuestMark& mark) {
  std::unique_ptr<Target>& target = targets[mark.key];
  if (!target) {
    target = std::make_unique<Target>();
    target->vtable = kTargetVtable;
    target->id = kTargetIdTag | (mark.key & kTargetIdMask) | (mark.key >> 63 << 55);
  }
  std::memcpy(target->position, mark.position, sizeof(target->position));
  return target.get();
}

void append(EntryVector* out, const QuestMark& mark) {
  const auto from_text = reinterpret_cast<StringFromText>(natives.string_from_text);
  Entry entry;
  from_text(&entry.quest, mark.name.c_str());
  from_text(&entry.objective, "");
  from_text(&entry.id, "");
  entry.type = mark.type;
  entry.objective_index = mark.objective;
  entry.target = target_of(mark);
  if (out->last == out->end) {
    reinterpret_cast<EntryInsert>(natives.entry_insert)(out, out->last, &entry);
  } else {
    reinterpret_cast<EntryCopy>(natives.entry_copy)(out->last, &entry);
    ++out->last;
  }
  reinterpret_cast<EntryFree>(natives.entry_free)(&entry);
}

EntryVector* __fastcall hooked_collect(void* quests, EntryVector* out) {
  if (!out) {
    return original_collect(quests, out);
  }
  EntryVector* result = out;
  if (natives.only_ours && natives.only_ours()) {
    // An empty vector, as the collector starts its own.
    out->first = nullptr;
    out->last = nullptr;
    out->end = nullptr;
  } else {
    result = original_collect(quests, out);
  }
  quest_marks(marks);
  for (const QuestMark& mark : marks) {
    append(out, mark);
  }
  static std::size_t logged = SIZE_MAX;
  if (log_line && marks.size() != logged) {
    logged = marks.size();
    log_line("MAP quest marks of ours: %zu", marks.size());
  }
  return result;
}

}  // namespace

bool install(const Natives& given, Logger logger) {
  natives = given;
  log_line = logger;
  if (!natives.collect || !natives.entry_copy || !natives.entry_insert || !natives.entry_free ||
      !natives.string_from_text) {
    return false;
  }
  if (MH_CreateHook(natives.collect, reinterpret_cast<void*>(&hooked_collect),
                    reinterpret_cast<void**>(&original_collect)) != MH_OK) {
    return false;
  }
  return MH_EnableHook(natives.collect) == MH_OK;
}

}  // namespace kcds::runtime::markers::map_quest_marks
