// Resource actions in the game's own key binding settings, WHGame 1.5.5.
//
// The settings' "Key bindings" screen lists C_Keybinds (playermodule). C_Keybinds loads
// libs/config/keybindSuperactions.xml once and parses it:
//
//   <ui_group name ui_label/>                     a category of the screen
//   <superaction name ui_group ui_name ui_tooltip keyboard="writeable|readable|hidden">
//     <action name map/>                           action-map actions it drives (none for ours)
//     <control input controller [slot]/>           its default inputs per device
//   </superaction>
//   <conflict name> <superaction name/>... </conflict>   inputs these may not share
//
// The parse inserts or updates entries by name and never clears the others, so KCDS parses its
// own small tree (ISystem::LoadXmlFromBuffer, vtable + 1040) whenever the resources' actions
// change. Each parse numbers its groups and superactions from 0; the KCDS ones are numbered after
// the game's afterwards. An action no longer wanted is parsed again as keyboard="hidden" with no
// inputs (the screen skips it). The game itself has no controller column: only keyboard and mouse.
//
// C_Keybinds:   + 16 superactions (MSVC unordered_map, list head at + 24; node: next + 0,
//               name + 24, order + 40, then per device 80 bytes apart: access + 80, default inputs
//               + 88, current inputs + 112, each a vector of CryString), + 80 groups (list head
//               + 88; node: name + 24, label + 32, order + 40).
// Apply (the profile's "keybinds_settings" tree): sets the current inputs of the
// named superactions and rebuilds the device's input maps; unknown names are skipped.
// Save (hooked): writes every superaction whose current inputs differ from their
// defaults into the profile. KCDS actions never reach the profile: during the save their current
// inputs read as their defaults. The game played without KCDS never meets them.
//
// Engine thread.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace kcds::runtime::keybinds::screen {

using Logger = void (*)(const char* format, ...);

struct Natives {
  void* (*services)() = nullptr;  // the game's services getter
  void* parse = nullptr;          // C_Keybinds parse, (hooked: finds C_Keybinds)
  void* apply = nullptr;          // C_Keybinds apply, (hooked: finds C_Keybinds)
  void* save = nullptr;           // C_Keybinds save, (hooked)
};

// A category of the screen.
struct Group {
  std::string name;   // kcds_..., also its label's text key
  std::string label;  // the text shown
};

// A resource action on the screen.
struct Action {
  std::string name;      // kcds_..., also its label's text key
  std::string group;     // a game group ("general", ...) or a Group's name
  std::string label;     // the text shown
  std::string keyboard;  // default keyboard or mouse input ("f6", "mouse4"), or ""
};

bool install(const Natives& natives, Logger logger);

// Whether the game's C_Keybinds is known: from its parse, profile load or save since injection,
// or else through the player module (services + 296, then + 64) once the game loaded its actions.
bool ready();

// Puts these groups and actions on the screen with their default inputs, and hides the KCDS
// actions shown before that are not among them.
void show(const std::vector<Group>& groups, const std::vector<Action>& actions);

// Sets the current keyboard inputs of KCDS actions (an empty list unbinds).
void bind(const std::map<std::string, std::vector<std::string>>& inputs);

// The current and default keyboard inputs of every KCDS action on the screen.
struct Binding {
  std::vector<std::string> current;
  std::vector<std::string> defaults;
};
std::map<std::string, Binding> bindings();

}  // namespace kcds::runtime::keybinds::screen
