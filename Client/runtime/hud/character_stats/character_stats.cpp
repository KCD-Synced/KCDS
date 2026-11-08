#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "character_stats.hpp"

#include <intrin.h>

#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "MinHook.h"
#include "kcd2s/integration/script_channel.hpp"
#include "stat_rows.hpp"

namespace kcds::runtime::hud::character_stats {
namespace {

namespace sdk = kcd2s::sdk;

// A row object (C_UIFlashCharacterSoulProperty): its id "<number>:<stat>" and label (CryStrings),
// the stat's levels and the shown comparison's, and whether a higher level is worse.
constexpr std::size_t kRowId = 8;
constexpr std::size_t kRowLabel = 16;
constexpr std::size_t kRowLevels[] = {40, 44, 48, 52};
constexpr std::size_t kRowInverted = 56;
// Stats the game leaves off the panel; a row of our own is not made from them.
constexpr std::uint32_t kStatsOffPanel[] = {86, 87, 150};
constexpr char kPanelArray[] = "SoulProperties";

// A shared_ptr of a row: the object and its control block.
struct RowRef {
  void* object;
  void* control;
};
struct RowList {
  RowRef* begin;
  RowRef* end;
  RowRef* capacity;
};
struct StatIds {
  const std::uint32_t* begin;
  const std::uint32_t* end;
};

using AddRows = std::int64_t(__fastcall*)(void* ui, int group, const StatIds* ids, void* soul,
                                          void* compared, RowList* rows);
using MakeRow = RowRef*(__fastcall*)(RowRef* out, const std::uint32_t* stat, void* soul,
                                     void* compared, const int* group);
using SetArray = std::int64_t(__fastcall*)(void* screen, const char* const* name,
                                           const RowList* rows);
using ControlCall = void(__fastcall*)(void* control);

Logger log_line = nullptr;
Natives natives;
AddRows original_add_rows = nullptr;
SetArray original_set_array = nullptr;

std::mutex rows_mutex;
StatRows wanted;
bool logged_rows = false;

// The souls and a stat of the panel being filled (engine thread), for rows of our own.
thread_local void* filling_soul = nullptr;
thread_local void* filling_compared = nullptr;
thread_local int filling_group = 0;
thread_local std::uint32_t filling_stat = 0;
thread_local bool filling_known = false;

template <class T>
T& field(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

bool shown_on_panel(std::uint32_t stat) {
  for (const std::uint32_t off : kStatsOffPanel) {
    if (stat == off) {
      return false;
    }
  }
  return true;
}

std::int64_t __fastcall hooked_add_rows(void* ui, int group, const StatIds* ids, void* soul,
                                        void* compared, RowList* rows) {
  if (ids) {
    for (const std::uint32_t* stat = ids->begin; stat != ids->end; ++stat) {
      if (shown_on_panel(*stat)) {
        filling_soul = soul;
        filling_compared = compared;
        filling_group = group;
        filling_stat = *stat;
        filling_known = true;
        break;
      }
    }
  }
  return original_add_rows(ui, group, ids, soul, compared, rows);
}

// The game's row made for the stat seen while filling, given the resource's label and value.
bool make_row(const StatRow& row, std::size_t index, RowRef& made) {
  made = {nullptr, nullptr};
  reinterpret_cast<MakeRow>(natives.make_row)(&made, &filling_stat, filling_soul, filling_compared,
                                              &filling_group);
  if (!made.object || !made.control) {
    return false;
  }
  const std::string id = "0:kcds_row_" + std::to_string(index);
  natives.string_assign(&field<char*>(made.object, kRowId), id.c_str());
  natives.string_assign(&field<char*>(made.object, kRowLabel), row.label.c_str());
  for (const std::size_t level : kRowLevels) {
    field<std::int32_t>(made.object, level) = row.value;
  }
  field<bool>(made.object, kRowInverted) = false;
  return true;
}

// Lets go of a row of our own as the game lets go of a shared_ptr.
void release(void* control) {
  auto* uses = reinterpret_cast<volatile long*>(static_cast<std::uint8_t*>(control) + 8);
  auto* weak = uses + 1;
  void** vtable = *static_cast<void***>(control);
  if (_InterlockedExchangeAdd(uses, -1) == 1) {
    reinterpret_cast<ControlCall>(vtable[0])(control);
    if (_InterlockedExchangeAdd(weak, -1) == 1) {
      reinterpret_cast<ControlCall>(vtable[1])(control);
    }
  }
}

const char* text_of(void* object, std::size_t offset) {
  const char* text = field<char*>(object, offset);
  return text ? text : "";
}

void log_game_rows(const RowList& rows) {
  if (!log_line || logged_rows) {
    return;
  }
  logged_rows = true;
  std::string line;
  for (const RowRef* row = rows.begin; row != rows.end; ++row) {
    if (!row->object) {
      continue;
    }
    line += " ";
    line += text_of(row->object, kRowId);
    line += "=";
    line += text_of(row->object, kRowLabel);
  }
  log_line("HUD character stats: the game's rows%s", line.c_str());
}

std::int64_t __fastcall hooked_set_array(void* screen, const char* const* name,
                                         const RowList* rows) {
  if (!name || !*name || std::strcmp(*name, kPanelArray) != 0 || !rows) {
    return original_set_array(screen, name, rows);
  }
  log_game_rows(*rows);
  StatRows set;
  {
    std::lock_guard<std::mutex> lock(rows_mutex);
    set = wanted;
  }
  const bool can_make = filling_known && natives.make_row && natives.string_assign;
  filling_known = false;
  if (!set.active) {
    return original_set_array(screen, name, rows);
  }
  std::vector<RowRef> shown;
  std::vector<void*> ours;
  std::size_t index = 0;
  const auto add_own = [&](const std::vector<StatRow>& own) {
    for (const StatRow& row : own) {
      RowRef made{};
      if (can_make && make_row(row, index, made)) {
        shown.push_back(made);
        ours.push_back(made.control);
      }
      ++index;
    }
  };
  add_own(set.top);
  for (const RowRef* row = rows->begin; row != rows->end; ++row) {
    if (row->object && is_hidden(set, text_of(row->object, kRowId))) {
      continue;
    }
    shown.push_back(*row);
  }
  add_own(set.bottom);
  RowList list{shown.data(), shown.data() + shown.size(), shown.data() + shown.size()};
  const std::int64_t result = original_set_array(screen, name, &list);
  for (void* control : ours) {
    release(control);
  }
  return result;
}

template <class Fn>
bool hook(void* target, void* detour, Fn* original) {
  return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

std::vector<StatRow> rows_of(const sdk::Value* list) {
  std::vector<StatRow> rows;
  if (!list || list->tag() != sdk::Tag::Array) {
    return rows;
  }
  for (const sdk::Value& entry : list->as_array()) {
    if (rows.size() >= kMostRows) {
      break;
    }
    const sdk::Value* label = kcd2s::integration::script_field(entry, "label");
    const sdk::Value* value = kcd2s::integration::script_field(entry, "value");
    if (!label || label->tag() != sdk::Tag::Str) {
      continue;
    }
    StatRow row;
    row.label = fitted_label(label->as_str());
    if (value && value->tag() == sdk::Tag::Int) {
      row.value = fitted_value(static_cast<double>(value->as_int()));
    } else if (value && value->tag() == sdk::Tag::Double) {
      row.value = fitted_value(value->as_double());
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

}  // namespace

bool install(const Natives& given, Logger log) {
  natives = given;
  log_line = log;
  return hook(natives.add_rows, reinterpret_cast<void*>(&hooked_add_rows), &original_add_rows) &&
         hook(natives.set_array, reinterpret_cast<void*>(&hooked_set_array), &original_set_array);
}

bool handle_runtime_message(const sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "hudCharacterStats") {
    return false;
  }
  StatRows next;
  const sdk::Value* rows = kcd2s::integration::script_field(message, "rows");
  if (rows && rows->tag() == sdk::Tag::Map) {
    next.active = true;
    const sdk::Value* hide = kcd2s::integration::script_field(*rows, "hide");
    if (hide && hide->tag() == sdk::Tag::Array) {
      for (const sdk::Value& name : hide->as_array()) {
        if (name.tag() == sdk::Tag::Str && next.hidden.size() < kMostHidden) {
          next.hidden.push_back(name.as_str());
        }
      }
    }
    next.top = rows_of(kcd2s::integration::script_field(*rows, "top"));
    next.bottom = rows_of(kcd2s::integration::script_field(*rows, "bottom"));
  }
  std::lock_guard<std::mutex> lock(rows_mutex);
  wanted = std::move(next);
  return true;
}

}  // namespace kcds::runtime::hud::character_stats
