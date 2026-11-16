#include "workpiece_guard.hpp"

#include <cstddef>
#include <cstdint>

#include "MinHook.h"

namespace kcds::runtime::crafting::workpiece_guard {
namespace {

// The lambda's captures: a pointer to the workpiece's entity pointer.
using Update = std::int64_t(__fastcall*)(void*** captures, float frame_seconds);
using GetCharacter = void*(__fastcall*)(void* entity, int slot);
constexpr std::size_t kGetCharacter = 101;

Update original_update = nullptr;
Logger log_line = nullptr;
bool logged = false;

std::int64_t __fastcall hooked_update(void*** captures, float frame_seconds) {
  void* entity = captures && *captures ? **captures : nullptr;
  void* character =
      entity ? (*reinterpret_cast<GetCharacter**>(entity))[kGetCharacter](entity, 0) : nullptr;
  if (!character) {
    if (!logged && log_line) {
      logged = true;
      log_line(
          "SMITHING the workpiece in the tongs has no character (its model is not a .cdf or did "
          "not load): its attachments are left as they are");
    }
    return 0;
  }
  return original_update(captures, frame_seconds);
}

}  // namespace

bool install(void* update, Logger logger) {
  log_line = logger;
  if (!update) {
    return false;
  }
  return MH_CreateHook(update, reinterpret_cast<void*>(&hooked_update),
                       reinterpret_cast<void**>(&original_update)) == MH_OK &&
         MH_EnableHook(update) == MH_OK;
}

}  // namespace kcds::runtime::crafting::workpiece_guard
