// Whether a key bound to an action of the game client is held, and the KCDS group of the game's key
// binding settings those actions show in.
#pragma once

#include <string>

namespace kcds::runtime::keybinds {

// The group of the game client's own actions (voice chat, pointing) in the key binding settings.
inline constexpr const char* kKcdsGroup = "kcds";
inline constexpr const char* kKcdsGroupLabel = "KCDS";

// The virtual key of an engine input name for the keyboard layout in use (a letter or digit by its
// place on the keyboard), 0 for a name this does not know.
int virtual_key_of(const std::string& name);

// Whether any input bound to the action (the player's choice, else its default) is held now.
bool held(const std::string& action);

}  // namespace kcds::runtime::keybinds
