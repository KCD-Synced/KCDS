// What KCDS remembers of the player's window mode, apart from the game's own settings.
//
// The game keeps its window mode in r_WindowType and saves it in its profile (attributes.xml,
// res_fs): 0 a normal window, 2 the menu's Fullscreen (a borderless full screen). The engine also
// knows 1, a borderless window of the chosen size, which only KCDS offers in the menu. A game
// started without KCDS must find the mode it had there, so the profile always keeps a mode of the
// game's own menu, and KCDS keeps the borderless window in its own file.
#pragma once

#include <string>

namespace kcds::runtime::display {

// The engine's r_WindowType values.
inline constexpr int kWindowed = 0;
inline constexpr int kBorderlessWindow = 1;
inline constexpr int kBorderlessFullScreen = 2;
inline constexpr int kExclusiveFullScreen = 3;

struct WindowModeMemory {
  // The player chose KCDS's borderless window.
  bool borderless = false;
  // The mode the game's profile keeps: the last one the player had without the borderless window.
  int vanilla = kBorderlessFullScreen;

  bool operator==(const WindowModeMemory& other) const {
    return borderless == other.borderless && vanilla == other.vanilla;
  }
};

// The game's profile is about to save `window_type` (res_fs): what it saves instead, and what KCDS
// remembers then.
struct ProfileSave {
  int saved = kBorderlessFullScreen;
  WindowModeMemory memory;
};
ProfileSave on_profile_save(int window_type, const WindowModeMemory& memory);

// The game has read its profile and user.cfg (r_WindowType is `window_type`): the memory with that
// mode as the vanilla one, and the r_WindowType KCDS starts the game in (`window_type` itself when
// the borderless window is not chosen).
struct GameStart {
  int window_type = kBorderlessFullScreen;
  WindowModeMemory memory;
};
GameStart on_game_start(int window_type, const WindowModeMemory& memory);

// The file's text and back. Text that is not the file's gives the default memory.
std::string to_text(const WindowModeMemory& memory);
WindowModeMemory from_text(const std::string& text);

}  // namespace kcds::runtime::display
