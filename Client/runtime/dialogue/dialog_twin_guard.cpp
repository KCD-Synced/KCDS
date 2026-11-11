#include "dialog_twin_guard.hpp"

#include <atomic>
#include <cstdint>

#include "MinHook.h"

namespace kcds::runtime::dialogue::twin_guard {
namespace {

// The dialogue, the person, the person's stand-in; the result is not read by the game's callers.
using SetUpTwin = std::uintptr_t(__fastcall*)(std::uintptr_t dialogue, std::uintptr_t person,
                                              std::uintptr_t twin);

SetUpTwin original_set_up_twin = nullptr;
Logger log_line = nullptr;
// Each person left out is told, up to this many times a game.
constexpr unsigned kToldMax = 20;
std::atomic<unsigned> told{0};

std::uintptr_t __fastcall guarded_set_up_twin(std::uintptr_t dialogue, std::uintptr_t person,
                                              std::uintptr_t twin) {
  if (twin == 0) {
    if (log_line && told.fetch_add(1) < kToldMax) {
      log_line("DIALOG a person without a dialogue stand-in left out (person %p)",
               reinterpret_cast<void*>(person));
    }
    return 0;
  }
  return original_set_up_twin(dialogue, person, twin);
}

}  // namespace

bool install(void* set_up_twin, Logger logger) {
  log_line = logger;
  if (!set_up_twin) {
    return false;
  }
  const bool hooked = MH_CreateHook(set_up_twin, reinterpret_cast<void*>(&guarded_set_up_twin),
                                    reinterpret_cast<void**>(&original_set_up_twin)) == MH_OK &&
                      MH_EnableHook(set_up_twin) == MH_OK;
  if (!hooked) {
    original_set_up_twin = nullptr;
  }
  return hooked;
}

}  // namespace kcds::runtime::dialogue::twin_guard
