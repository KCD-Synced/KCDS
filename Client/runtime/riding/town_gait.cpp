#include "town_gait.hpp"

#include <atomic>

#include "MinHook.h"

namespace kcds::runtime::riding::town_gait {
namespace {

using LimiterSwitch = void(__fastcall*)(void* owner, bool on);

LimiterSwitch original_switch = nullptr;
Logger log_line = nullptr;
std::atomic<bool> limited{true};

void __fastcall hooked_switch(void* owner, bool on) {
  const bool lifted = on && !limited.load();
  static int told = 0;
  if (log_line && told < 20) {
    ++told;
    log_line("RIDING horse speed limiter %s%s", on ? "on" : "off",
             lifted ? " asked: lifted (server horseTownLimit off)" : "");
  }
  original_switch(owner, lifted ? false : on);
}

}  // namespace

bool install(void* limiter_switch, Logger logger) {
  log_line = logger;
  return limiter_switch &&
         MH_CreateHook(limiter_switch, reinterpret_cast<void*>(&hooked_switch),
                       reinterpret_cast<void**>(&original_switch)) == MH_OK &&
         MH_EnableHook(limiter_switch) == MH_OK;
}

void set_limited(bool value) {
  limited.store(value);
}

}  // namespace kcds::runtime::riding::town_gait
