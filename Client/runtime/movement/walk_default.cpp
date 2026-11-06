#include "walk_default.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace kcds::runtime::movement::walk_default {
namespace {

// The actor's player input, its actor, and the walk/jog toggle in it.
constexpr std::size_t kActorPlayerInput = 2952;
constexpr std::size_t kPlayerInputActor = 16;
constexpr std::size_t kInputJogToggle = 124;
constexpr std::uint8_t kWalk = 0;
// How long after a sprint's key is released the walk is still put back (the sprint winds down).
constexpr ULONGLONG kAfterSprintMs = 1000;
// Input modes.
constexpr unsigned kPress = 1;
constexpr unsigned kRelease = 2;

Logger log_line = nullptr;
void* body = nullptr;         // the actor the preference was last applied to
bool preferred = false;       // the preference held last frame
void* known_actor = nullptr;  // the local actor of the last frame, for on_action
bool sprinting = false;
bool walked_before_sprint = false;
ULONGLONG walk_back_until = 0;  // after a sprint: put the walk back until then

// The toggle byte of the actor's player input, nullptr when the input is not the actor's.
std::uint8_t* toggle_of(void* actor) {
  if (!actor) {
    return nullptr;
  }
  auto input =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + kActorPlayerInput);
  if (!input || *reinterpret_cast<void**>(input + kPlayerInputActor) != actor) {
    return nullptr;
  }
  return input + kInputJogToggle;
}

void walk(std::uint8_t* toggle, const char* why) {
  if (*toggle == kWalk) {
    return;
  }
  *toggle = kWalk;
  if (log_line) {
    log_line("MOVEMENT walk by default: walking %s", why);
  }
}

}  // namespace

void install(Logger logger) {
  log_line = logger;
}

void tick(void* actor, bool walk_by_default) {
  known_actor = actor;
  std::uint8_t* toggle = toggle_of(actor);
  if (!toggle) {
    body = nullptr;
    preferred = false;
    return;
  }
  const bool new_body = actor != body;
  body = actor;
  if (walk_by_default && (!preferred || new_body)) {
    walk(toggle, new_body ? "(a new body)" : "(the rule arrived)");
  }
  preferred = walk_by_default;
  if (walk_back_until && GetTickCount64() > walk_back_until) {
    walk_back_until = 0;
  }
  // Once: the player's own toggle after that is theirs.
  if (walk_back_until && walk_by_default && walked_before_sprint && *toggle != kWalk) {
    walk(toggle, "again after the sprint");
    walk_back_until = 0;
  }
}

void on_action(const char* action, unsigned mode) {
  if (!action || std::strcmp(action, "sprint") != 0) {
    return;
  }
  std::uint8_t* toggle = toggle_of(known_actor);
  if (mode == kPress && !sprinting) {
    sprinting = true;
    walk_back_until = 0;
    walked_before_sprint = toggle && *toggle == kWalk;
  } else if (mode == kRelease && sprinting) {
    sprinting = false;
    walk_back_until = GetTickCount64() + kAfterSprintMs;
  }
}

}  // namespace kcds::runtime::movement::walk_default
