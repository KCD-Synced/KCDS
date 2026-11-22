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

void MinHookBackend::save(void* target) {
  const auto from = reinterpret_cast<std::uintptr_t>(target) - kSavedBefore;
  std::array<std::uint8_t, kSavedBytes> bytes{};
  std::memcpy(bytes.data(), reinterpret_cast<const void*>(from), kSavedBytes);
  std::lock_guard<std::mutex> lock(saved_mutex_);
  saved_.emplace(from, bytes);
}

int MinHookBackend::enable(void* target) {
  save(target);
  return MH_EnableHook(target);
}

int MinHookBackend::queue_enable(void* target) {
  save(target);
  return MH_QueueEnableHook(target);
}

std::uint32_t MinHookBackend::restore_original_bytes(const void* base, std::uint64_t size,
                                                     std::uint8_t* copy) {
  const auto start = reinterpret_cast<std::uintptr_t>(base);
  std::uint32_t restored = 0;
  std::lock_guard<std::mutex> lock(saved_mutex_);
  for (const auto& [from, bytes] : saved_) {
    if (from >= start && from + kSavedBytes <= start + size) {
      std::memcpy(copy + (from - start), bytes.data(), kSavedBytes);
      ++restored;
    }
  }
  return restored;
}

bool MinHookBackend::write_pointer(void** where, void* value) {
  DWORD old = 0;
  if (!VirtualProtect(where, sizeof(void*), PAGE_READWRITE, &old)) {
    return false;
  }
  *where = value;
  DWORD unused = 0;
  VirtualProtect(where, sizeof(void*), old, &unused);
  return true;
}

int MinHookBackend::apply_queued() {
  return MH_ApplyQueued();
}

}  // namespace kcds::loader
