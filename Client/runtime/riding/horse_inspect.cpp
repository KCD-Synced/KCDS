#include "horse_inspect.hpp"

#include <atomic>
#include <cstdint>

#include "kcd2s/integration/script_channel.hpp"

// After the headers above: MinHook brings in windows.h and its min and max macros.
#include "MinHook.h"

namespace kcds::runtime::riding::horse_inspect {
namespace {

using Open = char(__fastcall*)(void* screen, std::uint32_t horse);
using Front = void*(__fastcall*)(void* container, std::uint8_t index);

Open original_open = nullptr;
Front original_front = nullptr;
// Whether the screen compares with the player's own horse (the game's default).
std::atomic<bool> compare{true};
// The screen is opening on this thread (the engine thread).
thread_local bool opening = false;

char __fastcall hooked_open(void* screen, std::uint32_t horse) {
  opening = true;
  const char opened = original_open(screen, horse);
  opening = false;
  return opened;
}

void* __fastcall hooked_front(void* container, std::uint8_t index) {
  if (opening && index == 0 && !compare.load()) {
    return nullptr;
  }
  return original_front(container, index);
}

template <class Fn>
bool hook(void* target, void* detour, Fn* original) {
  return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

void install(const Natives& natives, Logger log) {
  const bool hooked =
      hook(natives.front, reinterpret_cast<void*>(&hooked_front), &original_front) &&
      hook(natives.open, reinterpret_cast<void*>(&hooked_open), &original_open);
  if (log) {
    log("HORSE inspect compare %s", hooked ? "hooked" : "not hooked");
  }
}

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  if (kcd2s::integration::script_operation(message) != "hudHorseCompare") {
    return false;
  }
  const kcd2s::sdk::Value* enabled = kcd2s::integration::script_field(message, "enabled");
  compare.store(!enabled || enabled->tag() != kcd2s::sdk::Tag::Bool || enabled->as_bool());
  return true;
}

}  // namespace kcds::runtime::riding::horse_inspect
