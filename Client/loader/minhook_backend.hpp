// The loader's hook backend on Windows: MinHook patches the game, and the thunks live in blocks of
// executable memory the loader never frees (a thunk outlives every payload).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <array>
#include <map>
#include <mutex>

#include "hook_registry.hpp"

namespace kcds::loader {

class MinHookBackend : public HookBackend {
 public:
  void* make_thunk(std::atomic<void*>** slot) override;
  int create(void* target, void* thunk, void** trampoline) override;
  int enable(void* target) override;
  int queue_enable(void* target) override;
  int apply_queued() override;
  bool write_pointer(void** where, void* value) override;
  int remove(void* target) override;

  // Writes the game's own bytes back where the hooks patched it, in copy (the size bytes at base).
  std::uint32_t restore_original_bytes(const void* base, std::uint64_t size, std::uint8_t* copy);

 private:
  // The bytes around a target before MinHook patched it: a jump at the target, or a short jump
  // there and a long one in the padding before it.
  static constexpr std::size_t kSavedBefore = 8;
  static constexpr std::size_t kSavedBytes = 24;
  void save(void* target);

  std::mutex saved_mutex_;
  std::map<std::uintptr_t, std::array<std::uint8_t, kSavedBytes>> saved_;
  std::uint8_t* block_ = nullptr;
  std::size_t used_ = 0;
  bool initialized_ = false;
};

}  // namespace kcds::loader
