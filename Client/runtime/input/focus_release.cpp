#include "focus_release.hpp"

#include <windows.h>

#include <cstring>
#include <vector>

namespace kcds::runtime::input::focus {

namespace {

// The game's movement actions (their interned names).
constexpr const char* kMovementActions[] = {"moveforward", "moveback", "moveleft", "moveright",
                                            "sprint"};

struct Held {
  void* input = nullptr;
  const char** action = nullptr;
};

std::vector<Held> held;
bool was_foreground = false;

bool foreground() {
  DWORD process = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &process);
  return process == GetCurrentProcessId();
}

bool movement(const char* name) {
  for (const char* candidate : kMovementActions) {
    if (std::strcmp(name, candidate) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

void on_action(void* input, const char** action, unsigned mode) {
  if (!input || !action || !*action || !movement(*action)) {
    return;
  }
  for (auto it = held.begin(); it != held.end(); ++it) {
    if (it->action == action) {
      held.erase(it);
      break;
    }
  }
  if (mode == 1 && foreground()) {
    held.push_back({input, action});
  }
}

void tick(Deliver deliver, Logger logger) {
  const bool now_foreground = foreground();
  if (was_foreground && !now_foreground && !held.empty() && deliver) {
    const std::vector<Held> releasing = held;
    held.clear();
    for (const Held& key : releasing) {
      deliver(key.input, key.action, 2, 0.0f);
      if (logger) {
        logger("INPUT %s let go: the game window lost the focus", *key.action);
      }
    }
  }
  was_foreground = now_foreground;
}

}  // namespace kcds::runtime::input::focus
