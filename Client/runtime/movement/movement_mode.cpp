#include "movement_mode.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

namespace kcds::runtime::movement {
namespace {

using SetRestriction = void(__fastcall*)(void* actor, bool allow_run, bool allow_sprint);

constexpr std::size_t kAllowSprint = 1033;
constexpr std::size_t kAllowRun = 1034;

Logger log_line = nullptr;
SetRestriction original_restriction = nullptr;
// The actor the rule holds, and what the game asked for while it held.
std::atomic<void*> held_actor{nullptr};
std::atomic<bool> wished_run{true};
std::atomic<bool> wished_sprint{true};

bool permission(void* actor, std::size_t offset) {
  return *(static_cast<std::uint8_t*>(actor) + offset) != 0;
}

void __fastcall hooked_restriction(void* actor, bool allow_run, bool allow_sprint) {
  if (actor && actor == held_actor.load()) {
    wished_run = allow_run;
    wished_sprint = allow_sprint;
    original_restriction(actor, false, false);
    return;
  }
  original_restriction(actor, allow_run, allow_sprint);
}

void hold(void* actor) {
  wished_run = permission(actor, kAllowRun);
  wished_sprint = permission(actor, kAllowSprint);
  held_actor = actor;
  original_restriction(actor, false, false);
  if (log_line) {
    log_line("MOVEMENT walk enforced (the game had run=%u sprint=%u)", unsigned(wished_run.load()),
             unsigned(wished_sprint.load()));
  }
}

void release(void* actor) {
  held_actor = nullptr;
  original_restriction(actor, wished_run, wished_sprint);
  if (log_line) {
    log_line("MOVEMENT walk no longer enforced (run=%u sprint=%u given back)",
             unsigned(wished_run.load()), unsigned(wished_sprint.load()));
  }
}

}  // namespace

bool install(const Natives& natives, Logger logger) {
  log_line = logger;
  if (!natives.restriction) {
    return false;
  }
  return MH_CreateHook(natives.restriction, reinterpret_cast<void*>(hooked_restriction),
                       reinterpret_cast<void**>(&original_restriction)) == MH_OK &&
         MH_EnableHook(natives.restriction) == MH_OK;
}

void tick(void* actor, bool walk_enforced) {
  void* held = held_actor.load();
  if (held && held != actor) {
    // A new body (or none): the old actor is gone with what it held.
    held_actor = nullptr;
    held = nullptr;
  }
  if (!actor) {
    return;
  }
  if (held && !walk_enforced) {
    release(actor);
    return;
  }
  if (!walk_enforced) {
    return;
  }
  if (!held) {
    hold(actor);
    return;
  }
  // The game re-allowed by writing the permissions itself (the end of carrying a body restores
  // what it saved): that is what it wants now, and the rule takes them away again.
  const bool run = permission(actor, kAllowRun);
  const bool sprint = permission(actor, kAllowSprint);
  if (run || sprint) {
    wished_run = run;
    wished_sprint = sprint;
    original_restriction(actor, false, false);
  }
}

}  // namespace kcds::runtime::movement
