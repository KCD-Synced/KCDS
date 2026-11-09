// The player's own key bindings of KCDS actions, kept on this computer for each
// server: %LOCALAPPDATA%\KCDS\keybinds\<server>.txt (KCDS's own actions: kcds.txt, for every
// server), one line per action and device:
//
//   <action name> TAB keyboard TAB <input>,<input>     (empty: unbound)
//   <action name> TAB controller TAB <input>
//
// An action without a line has its resource's default. The game's own profile never holds them.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace kcds::runtime::keybinds {

struct Choice {
  std::optional<std::vector<std::string>> keyboard;
  std::optional<std::string> controller;
};

using Choices = std::map<std::string, Choice>;

// The file of a server ("host:port" or its listing id).
std::wstring store_path(const std::string& server);
// The file of KCDS's own actions (voice chat's keys), kcds.txt: the same on every server.
std::wstring builtin_store_path();

Choices load_choices(const std::wstring& path);
bool save_choices(const std::wstring& path, const Choices& choices);

}  // namespace kcds::runtime::keybinds
