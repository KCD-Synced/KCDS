#include "needs.hpp"

#include <cstddef>

namespace kcds::runtime::needs {
namespace {

using GetCVar = void*(__fastcall*)(void* console, const char* name);
using CVarGetInt = int(__fastcall*)(void* cvar);
using CVarSetInt = void(__fastcall*)(void* cvar, int value);

constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarGetInt = 2;
constexpr std::size_t kCVarSetInt = 7;
constexpr char kSkipNeedTimers[] = "wh_rpg_SkipFoodTimers";

void** console_slot = nullptr;
Logger log_line = nullptr;
// What the server wants, and whether this client has the game's need timers stopped.
bool enabled = true;
bool timers_stopped = false;
bool was_in_world = false;
// The server's fill missed a body that was not awake when the needs went off (set_enabled).
bool fill_kept = false;
// A console without the cvar is reported once and left alone.
bool unavailable = false;

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*reinterpret_cast<void***>(object))[index]);
}

// Sets wh_rpg_SkipFoodTimers through the console. False when the console or the cvar is missing.
bool stop_need_timers(bool stop) {
  void* console = console_slot ? *console_slot : nullptr;
  void* cvar =
      console ? slot<GetCVar>(console, kConsoleGetCVar)(console, kSkipNeedTimers) : nullptr;
  if (!cvar) {
    if (log_line) {
      log_line("NEEDS %s unavailable; the needs keep running", kSkipNeedTimers);
    }
    return false;
  }
  slot<CVarSetInt>(cvar, kCVarSetInt)(cvar, stop ? 1 : 0);
  if (log_line) {
    log_line("NEEDS %s by the server (%s=%d)", stop ? "off" : "on", kSkipNeedTimers,
             slot<CVarGetInt>(cvar, kCVarGetInt)(cvar));
  }
  return true;
}

}  // namespace

void install(void** console, Logger logger) {
  console_slot = console;
  log_line = logger;
}

void set_enabled(bool now_enabled, bool body_awake) {
  if (enabled && !now_enabled && !body_awake) {
    fill_kept = true;
    if (log_line) {
      log_line("NEEDS off while the body is not awake; filled when it is");
    }
  }
  if (now_enabled) {
    fill_kept = false;
  }
  enabled = now_enabled;
}

bool tick(bool in_world, bool body_awake) {
  if (was_in_world && !in_world) {
    // Out of a world the server's word is gone.
    enabled = true;
    fill_kept = false;
  }
  was_in_world = in_world;
  bool fill_due = false;
  if (fill_kept && in_world && !enabled && body_awake) {
    fill_kept = false;
    fill_due = true;
    if (log_line) {
      log_line("NEEDS the body is awake; the kept fill is applied");
    }
  }
  // The timers stop only in a world whose server has the needs off.
  const bool stop = in_world && !enabled;
  if (stop == timers_stopped || unavailable) {
    return fill_due;
  }
  if (!stop_need_timers(stop)) {
    unavailable = true;
    return fill_due;
  }
  timers_stopped = stop;
  return fill_due;
}

}  // namespace kcds::runtime::needs
