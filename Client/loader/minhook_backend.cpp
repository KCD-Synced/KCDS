#include "minhook_backend.hpp"

#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::loader {
namespace {

// A block of thunks: the code first, the slots after it, 8-byte aligned so a slot is written in
// one store while another thread jumps through it.
constexpr std::size_t kBlockBytes = 64 * 1024;
constexpr std::size_t kThunkBytes = 16;
constexpr std::size_t kThunks = kBlockBytes / 2 / kThunkBytes;
constexpr std::size_t kSlotsAt = kBlockBytes / 2;

}  // namespace

void* MinHookBackend::make_thunk(std::atomic<void*>** slot) {
  if (!block_ || used_ == kThunks) {
    block_ = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, kBlockBytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    used_ = 0;
    if (!block_) {
      return nullptr;
    }
  }
  std::uint8_t* code = block_ + used_ * kThunkBytes;
  auto* where = reinterpret_cast<std::atomic<void*>*>(block_ + kSlotsAt + used_ * sizeof(void*));
  ++used_;
  // jmp qword ptr [rip + disp32]: the slot.
  const auto disp = static_cast<std::int32_t>(reinterpret_cast<std::uint8_t*>(where) - (code + 6));
  code[0] = 0xFF;
  code[1] = 0x25;
  std::memcpy(code + 2, &disp, sizeof(disp));
  std::memset(code + 6, 0xCC, kThunkBytes - 6);
  FlushInstructionCache(GetCurrentProcess(), code, kThunkBytes);
  *slot = where;
  return code;
}

int MinHookBackend::create(void* target, void* thunk, void** trampoline) {
  if (!initialized_) {
    const MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
      return status;
    }
    initialized_ = true;
  }
  return MH_CreateHook(target, thunk, trampoline);
}

int MinHookBackend::enable(void* target) {
  return MH_EnableHook(target);
}

int MinHookBackend::queue_enable(void* target) {
  return MH_QueueEnableHook(target);
}

int MinHookBackend::apply_queued() {
  return MH_ApplyQueued();
}

}  // namespace kcds::loader
