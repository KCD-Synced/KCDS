#include "seated_body.hpp"

#include <cstddef>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

namespace kcds::runtime::riding {
namespace {

using SetBodyPosition = std::int64_t(__fastcall*)(void* state, unsigned position, unsigned second,
                                                  void* linked);

// The posture state (an actor's +0x990): its mount state (IsMounted is state 2)
// and the horse body position.
constexpr std::size_t kMountState = 32;
constexpr std::uint8_t kMounted = 2;
constexpr unsigned kHorseBodyPosition = 5;

Logger log_line = nullptr;
SetBodyPosition original_set_body_position = nullptr;

std::int64_t __fastcall kept_seated(void* state, unsigned position, unsigned second, void* linked) {
  const auto mount_state = *(static_cast<const std::uint8_t*>(state) + kMountState);
  if (mount_state == kMounted && position != kHorseBodyPosition) {
    if (log_line) {
      log_line("RIDING kept seated: body position %u refused while mounted", position);
    }
    return 0;
  }
  return original_set_body_position(state, position, second, linked);
}

}  // namespace

bool keep_seated_riders(void* set_body_position, Logger logger) {
  log_line = logger;
  if (!set_body_position) {
    return false;
  }
  return MH_CreateHook(set_body_position, reinterpret_cast<void*>(kept_seated),
                       reinterpret_cast<void**>(&original_set_body_position)) == MH_OK &&
         MH_EnableHook(set_body_position) == MH_OK;
}

}  // namespace kcds::runtime::riding
