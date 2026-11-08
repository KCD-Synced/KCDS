// The inventory's character panel (ApseCharacter): a client resource hides the game's stats there
// and adds rows of its own (Hud.setCharacterStats, the runtime's op 'hudCharacterStats').
//
// The game fills the panel each time it opens: it makes one row object per stat
// (C_UIFlashCharacterSoulProperty, a label at + 16 and the levels at + 40 to + 52), hands the list
// to the screen as its "SoulProperties" array and then calls SetSoulProperties. The list is only
// filled for that hand-over, so a script cannot edit the panel later. This module changes the list
// at the hand-over: the game's rows a resource hides are left out, and rows of its own, made by the
// game's own row maker and given the resource's label and value, go above and below. The game's
// list itself is not changed. Engine thread; the runtime message on the network thread only stores
// the rows.
#pragma once

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::hud::character_stats {

using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void* string, const char* text);

struct Natives {
  void* add_rows = nullptr;   // (ui, group, stat ids, soul, soul, rows): a group's rows
  void* make_row = nullptr;   // (out, stat id, soul, soul, group): one row object
  void* set_array = nullptr;  // (screen, name, rows): a list of rows as a screen's array
  StringAssign string_assign = nullptr;
};

bool install(const Natives& natives, Logger log);

// { op: "hudCharacterStats", rows: null | { hide, top, bottom } }; true when it was this module's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

}  // namespace kcds::runtime::hud::character_stats
