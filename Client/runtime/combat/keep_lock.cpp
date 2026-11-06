// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "keep_lock.hpp"

#include "../core/log.hpp"
#include "combat_identity.hpp"

#include <array>

namespace kcds::game_client {

// Logs the local player's lock state whenever it changes, from a relayed hit until two seconds
// after it, to find what makes the lock marker blink; engine thread.
void trace_lock_state(std::uint8_t* movement, std::uint8_t* controller, std::uint8_t* core) {
  static std::array<std::uintptr_t, 6> last{};
  if (GetTickCount64() >= keep_lock_until.load() + 1100) {
    last = {};
    return;
  }
  float timer = 0;
  if (controller) {
    std::memcpy(&timer, controller + 176, 4);
  }
  const std::array<std::uintptr_t, 6> state{
      controller ? controller[kHardLockFlag] : 0u,
      core ? reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(core + 3312)) : 0u,
      core ? reinterpret_cast<std::uintptr_t>(*reinterpret_cast<void**>(core + 3320)) : 0u,
      native_action_type(movement),
      timer > 0 ? 1u : 0u,
      controller ? controller[193] : 0u,
  };
  if (state == last) {
    return;
  }
  last = state;
  log("COMBAT LOCK trace hard=%u combat_target=%p lock_target=%p action=%u timer=%.2f "
      "byte193=%u",
      unsigned(state[0]), reinterpret_cast<void*>(state[1]), reinterpret_cast<void*>(state[2]),
      unsigned(state[3]), timer, unsigned(state[5]));
}

// Keeps the hard lock through a relayed hit's reaction; engine thread.
void tick_keep_lock(void* actor) {
  if (!actor) {
    return;
  }
  auto movement = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(actor) + 632);
  auto controller = movement ? *reinterpret_cast<std::uint8_t**>(movement + 1104) : nullptr;
  auto core = movement ? *reinterpret_cast<std::uint8_t**>(movement + 528) : nullptr;
  if (movement) {
    trace_lock_state(movement, controller, core);
  }
  if (!keep_hard_lock.load() || GetTickCount64() >= keep_lock_until.load()) {
    return;
  }
  // Only while the target itself is still selected (the clear is skipped in the window).
  if (controller && core && *reinterpret_cast<void**>(core + 3320) && !controller[kHardLockFlag]) {
    controller[kHardLockFlag] = 1;
    log("COMBAT TARGET hard lock kept after a relayed hit actor=%p", actor);
  }
}

}  // namespace kcds::game_client
