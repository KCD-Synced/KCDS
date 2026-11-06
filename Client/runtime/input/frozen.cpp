#include "frozen.hpp"

#include <cstring>

namespace kcds::runtime::input::frozen {
namespace {

// The local player's actions a frozen player keeps: looking around (the rotate actions) and the
// pause menu.
constexpr const char* kKeptPrefixes[] = {"rotate", "menu", "pause"};
constexpr unsigned kReleased = 2;

bool held = false;

}  // namespace

void set_frozen(bool frozen) {
  held = frozen;
}

bool frozen() {
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

}  // namespace kcds::runtime::input::frozen
