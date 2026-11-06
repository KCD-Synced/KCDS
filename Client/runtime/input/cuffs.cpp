#include "cuffs.hpp"

#include <cstring>

namespace kcds::runtime::input::cuffs {
namespace {

// The local player's actions a cuffed player keeps: walking (the move actions and the walk and
// run toggles) and looking around (the rotate actions), and the pause menu.
constexpr const char* kKeptPrefixes[] = {"move", "rotate", "walk", "menu", "pause"};
constexpr unsigned kReleased = 2;

bool held = false;

}  // namespace

void set_cuffed(bool cuffed) {
  held = cuffed;
}

bool cuffed() {
  return held;
}

bool refuse_action(const char* action, unsigned mode) {
  if (!held || !action || mode == kReleased) {
    return false;
  }
  for (const char* prefix : kKeptPrefixes) {
    if (std::strncmp(action, prefix, std::strlen(prefix)) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace kcds::runtime::input::cuffs
