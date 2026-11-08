#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "character_stats.hpp"

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
// The value as text, which the screen shows for the derived stats (speed, noise and the like),
// and the shown comparison's.
constexpr std::size_t kRowValueTexts[] = {24, 32};
constexpr std::size_t kRowLevels[] = {40, 44, 48, 52};
constexpr std::size_t kRowInverted = 56;
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

using SetArray = std::int64_t(__fastcall*)(void* screen, const char* const* name,
                                           const RowList* rows);

Logger log_line = nullptr;
Natives natives;
SetArray original_set_array = nullptr;

std::mutex rows_mutex;
StatRows wanted;
bool logged_rows = false;

template <class T>
T& field(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

const char* text_of(void* object, std::size_t offset) {
  const char* text = field<char*>(object, offset);
  return text ? text : "";
}

// The panel's rows once in the log, for the stat names a resource hides or sets.
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

void show(void* object, const StatRow& row) {
  natives.string_assign(&field<char*>(object, kRowLabel), row.label.c_str());
  const std::string value = std::to_string(row.value);
  for (const std::size_t text : kRowValueTexts) {
    natives.string_assign(&field<char*>(object, text), value.c_str());
  }
  for (const std::size_t level : kRowLevels) {
    field<std::int32_t>(object, level) = row.value;
  }
  field<bool>(object, kRowInverted) = false;
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
  if (!set.active) {
    return original_set_array(screen, name, rows);
  }
  std::vector<RowRef> shown;
  for (const RowRef* row = rows->begin; row != rows->end; ++row) {
    if (!row->object) {
      shown.push_back(*row);
      continue;
    }
    const char* id = text_of(row->object, kRowId);
    if (is_hidden(set, id)) {
      continue;
    }
    if (const StatRow* own = shown_for(set, id)) {
      show(row->object, *own);
    }
    shown.push_back(*row);
  }
  RowList list{shown.data(), shown.data() + shown.size(), shown.data() + shown.size()};
  return original_set_array(screen, name, &list);
}

std::vector<StatRow> rows_of(const sdk::Value* list) {
  std::vector<StatRow> rows;
  if (!list || list->tag() != sdk::Tag::Array) {
    return rows;
  }
  for (const sdk::Value& entry : list->as_array()) {
    if (rows.size() >= kMostStats) {
      break;
    }
    const sdk::Value* stat = kcd2s::integration::script_field(entry, "stat");
    const sdk::Value* label = kcd2s::integration::script_field(entry, "label");
    const sdk::Value* value = kcd2s::integration::script_field(entry, "value");
    if (!stat || stat->tag() != sdk::Tag::Str || !label || label->tag() != sdk::Tag::Str) {
      continue;
    }
    StatRow row;
    row.stat = stat->as_str();
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
  if (!natives.set_array || !natives.string_assign) {
    return false;
  }
  return MH_CreateHook(natives.set_array, reinterpret_cast<void*>(&hooked_set_array),
                       reinterpret_cast<void**>(&original_set_array)) == MH_OK &&
         MH_EnableHook(natives.set_array) == MH_OK;
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
        if (name.tag() == sdk::Tag::Str && next.hidden.size() < kMostStats) {
          next.hidden.push_back(name.as_str());
        }
      }
    }
    next.shown = rows_of(kcd2s::integration::script_field(*rows, "set"));
  }
  std::lock_guard<std::mutex> lock(rows_mutex);
  wanted = std::move(next);
  return true;
}

}  // namespace kcds::runtime::hud::character_stats
