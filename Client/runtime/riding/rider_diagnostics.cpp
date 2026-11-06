#include "rider_diagnostics.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"

namespace kcds::game_client {

// diagnostics: a body position (1 standing ... 5 horse) set on any actor
// (positions object and the new value) and the horse stand-up of the local player
// (called with an action whose +8 is the actor) are logged with their native callers
// while the local player rides; the seated rider plays the on-foot clips when it stands.
BodyPositionSet original_body_position_set = nullptr;
BodyPositionStand original_body_position_stand = nullptr;
std::atomic<bool> local_rider_mounted{false};

void log_native_callers(const char* tag) {
  void* stack[16]{};
  const auto count = CaptureStackBackTrace(2, 16, stack, nullptr);
  for (unsigned i = 0; i < count; ++i) {
    const auto at = reinterpret_cast<std::uintptr_t>(stack[i]);
    const auto base = reinterpret_cast<std::uintptr_t>(image);
    if (at >= base && at - base < image_size) {
      log("%s caller frame=%u ida=%llx", tag, i, 0x180000000ULL + at - base);
    }
  }
}

std::int64_t __fastcall hooked_body_position_set(void* positions, const int* position) {
  if (local_rider_mounted.load() && position) {
    log("RIDER body position set positions=%p value=%d", positions, *position);
    log_native_callers("RIDER body position set");
  }
  return original_body_position_set(positions, position);
}

void __fastcall hooked_body_position_stand(void* action, int result) {
  auto actor = action ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(action) + 8) : nullptr;
  if (actor && actor == local_combat_actor.load()) {
    log("RIDER body position stand-up local result=%d", result);
    log_native_callers("RIDER body position stand-up");
  }
  original_body_position_stand(action, result);
}

}  // namespace kcds::game_client
