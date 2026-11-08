#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "character_stats.hpp"

#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../../native_ui/native_ui.hpp"
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
using CallScreen = char(__fastcall*)(void* screen, const char* const* name);

// The screen's call that lays the rows out, and its element instance (screen + 72).
constexpr char kPanelCall[] = "SetSoulProperties";
constexpr std::size_t kScreenElement = 72;
// The stats' clips (soulProperty<n>), each column top to bottom (ApseCharacter.gfx).
constexpr char kClipPrefix[] = "_root.main.stats.soulProperties.soulProperty";
struct StatClip {
  const char* stat;
  int clip;
};
constexpr StatClip kColumns[2][6] = {
    {{"speech", 12}, {"charisma", 4}, {"rsb", 5}, {"con", 7}, {"noi", 8}, {"evi", 6}},
    {{"strength", 9},
     {"agility", 10},
     {"vitality", 11},
     {"health", 1},
     {"exhaust", 2},
     {"hunger", 3}},
};

Logger log_line = nullptr;
Natives natives;
SetArray original_set_array = nullptr;
CallScreen original_call_screen = nullptr;

std::mutex rows_mutex;
StatRows wanted;
bool logged_rows = false;
// Where the screen put each clip of an element before this module moved any (engine thread).
void* homes_element = nullptr;
std::map<int, native_ui::ClipPlace> homes;

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

std::string clip_name(int clip) {
  return kClipPrefix + std::to_string(clip);
}

// The panel's clips after the screen laid its rows out: a hidden stat's clip away, icon and all,
// and the clips below it in its column up into the places above. With nothing hidden every clip
// goes back where the screen put it.
void arrange(void* element, const StatRows& set) {
  const bool hiding = set.active && !set.hidden.empty();
  if (!element) {
    return;
  }
  if (element != homes_element) {
    if (!hiding) {
      return;
    }
    homes_element = element;
    homes.clear();
  }
  for (const auto& column : kColumns) {
    constexpr int kRows = static_cast<int>(sizeof(column) / sizeof(column[0]));
    native_ui::ClipPlace slots[kRows];
    for (int row = 0; row < kRows; ++row) {
      auto home = homes.find(column[row].clip);
      if (home == homes.end()) {
        native_ui::ClipPlace place;
        if (!native_ui::element_clip(element, clip_name(column[row].clip), place)) {
          return;
        }
        home = homes.emplace(column[row].clip, place).first;
      }
      slots[row] = home->second;
    }
    int next = 0;
    for (int row = 0; row < kRows; ++row) {
      native_ui::ClipPlace place = slots[row];
      if (hiding && is_hidden(set, column[row].stat)) {
        place.visible = false;
      } else {
        place.x = slots[next].x;
        place.y = slots[next].y;
        ++next;
      }
      native_ui::place_element_clip(element, clip_name(column[row].clip), place);
    }
  }
}

char __fastcall hooked_call_screen(void* screen, const char* const* name) {
  const char result = original_call_screen(screen, name);
  if (screen && name && *name && std::strcmp(*name, kPanelCall) == 0) {
    StatRows set;
    {
      std::lock_guard<std::mutex> lock(rows_mutex);
      set = wanted;
    }
    arrange(field<void*>(screen, kScreenElement), set);
  }
  return result;
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
  if (!natives.set_array || !natives.call_screen || !natives.string_assign) {
    return false;
  }
  return MH_CreateHook(natives.set_array, reinterpret_cast<void*>(&hooked_set_array),
                       reinterpret_cast<void**>(&original_set_array)) == MH_OK &&
         MH_EnableHook(natives.set_array) == MH_OK &&
         MH_CreateHook(natives.call_screen, reinterpret_cast<void*>(&hooked_call_screen),
                       reinterpret_cast<void**>(&original_call_screen)) == MH_OK &&
         MH_EnableHook(natives.call_screen) == MH_OK;
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
