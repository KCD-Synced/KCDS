// The loader's hook backend on Windows: MinHook patches the game, and the thunks live in blocks of
// executable memory the loader never frees (a thunk outlives every payload).
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "hook_registry.hpp"

namespace kcds::loader {

class MinHookBackend : public HookBackend {
 public:
  void* make_thunk(std::atomic<void*>** slot) override;
  int create(void* target, void* thunk, void** trampoline) override;
  int enable(void* target) override;
  int queue_enable(void* target) override;
  int apply_queued() override;

 private:
  std::uint8_t* block_ = nullptr;
  std::size_t used_ = 0;
  bool initialized_ = false;
};

}  // namespace kcds::loader
