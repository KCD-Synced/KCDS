// Which KCDS actions go into the key binding screen's conflict group.
//
// The game makes every member of a conflict group a conflict of every other member, and on
// confirm it reports an overlap when two of them share a key. A server may give several of its
// actions one default key on purpose (each acts in its own place), so such actions stay out of the
// group: with them in it, every confirm reported an overlap whatever key the player changed. An
// action without a default key stays out too.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace kcds::runtime::keybinds::conflicts {

struct Member {
  std::string name;
  std::string key;  // its default keyboard or mouse input, or ""
};

// The names of the actions whose default key no other action has, in the order given.
inline std::vector<std::string> group_of(const std::vector<Member>& actions) {
  std::map<std::string, int> users;
  for (const Member& action : actions) {
    if (!action.key.empty()) {
      ++users[action.key];
    }
  }
  std::vector<std::string> names;
  for (const Member& action : actions) {
    if (!action.key.empty() && users[action.key] == 1) {
      names.push_back(action.name);
    }
  }
  return names;
}

}  // namespace kcds::runtime::keybinds::conflicts
