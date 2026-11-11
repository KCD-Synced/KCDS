#include "presented_frame.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

namespace kcds::runtime::nameplates::presented_frame {
namespace {

// The frame number the engine thread hands the render thread, in the renderer.
constexpr std::size_t kHandedFrame = 0x29C0;

std::atomic<void* const*> renderer_global{nullptr};

}  // namespace

void install(void* const* renderer) {
  renderer_global.store(renderer);
}

std::int64_t current() {
  void* const* global = renderer_global.load();
  if (!global) {
    return -1;
  }
  __try {
    void* renderer = *global;
    if (!renderer) {
      return -1;
    }
    const auto frame = *reinterpret_cast<const volatile std::int32_t*>(
        static_cast<std::uint8_t*>(renderer) + kHandedFrame);
    return frame < 0 ? -1 : std::int64_t(frame);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return -1;
  }
}

std::int64_t next() {
  const std::int64_t frame = current();
  return frame < 0 ? -1 : frame + 1;
}

}  // namespace kcds::runtime::nameplates::presented_frame
