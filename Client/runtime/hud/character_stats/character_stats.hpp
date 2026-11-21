// The inventory's character panel (ApseCharacter): a client resource hides the game's stats there
// and shows its own label and value in a stat's place (Hud.setCharacterStats, the runtime's op
// 'hudCharacterStats').
//
// The game fills the panel each time it opens: it makes one row object per stat
// (C_UIFlashCharacterSoulProperty: its id "<number>:<stat>" at + 8, its label at + 16 and the
// levels at + 40 to + 52), hands the list to the screen as its "SoulProperties" array and then
// calls SetSoulProperties. The screen puts each row in the place of its stat. The list is only
// filled for that hand-over, so a script cannot edit the panel later. This module changes it at
// the hand-over: the game's rows a resource hides are left out of a list of its own, and a stat's
// row the resource sets gets its label and value (the rows are made new for each hand-over).
// Engine thread; the runtime message on the network thread only stores the setting.
#pragma once

#include <cstddef>
#include <cstdint>

#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::hud::character_stats {

using Logger = void (*)(const char* format, ...);
using StringAssign = void*(__fastcall*)(void* string, const char* text);

struct Natives {
  void* set_array = nullptr;    // (screen, name, rows): a list of rows as a screen's array
  void* call_screen = nullptr;  // (screen, name): a call of a screen's function
  StringAssign string_assign = nullptr;
  // The character panels' refreshes, each (panel): fill the panel again from the stats. [0] the
  // inventory's panel (C_UIShopCharacter, vtable slot 19), [1] the Player screen's
  // (C_UIApseCharacter, slot 19). Found by signature: the classes' RTTI found another of their
  // vtables, whose slot 19 the inventory never called.
  void* refreshes[2] = {};
};

bool install(const Natives& natives, Logger log);

// { op: "hudCharacterStats", rows: null | { hide, set } }; true when it was this module's.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Each frame (engine thread): a setting that came while the inventory is open shows at once, by
// the panel's own refresh (a script's hunger or thirst changed by eating showed only after the
// inventory was closed and opened again). `menu_open`: one of the game's screens is open.
void tick(bool menu_open);

// The player's stats changed while the inventory may be open (an item eaten there, a vital the
// server changed): the open panel is filled again a moment later, once the change has taken; it
// showed the old values until the inventory was closed and opened again. Any thread.
void refresh_after_change();

}  // namespace kcds::runtime::hud::character_stats
