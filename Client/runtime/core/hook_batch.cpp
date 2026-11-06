// The one source that calls MinHook's own MH_EnableHook (see hook_batch.hpp).
#undef MH_EnableHook

// Winsock before Windows.h (see client_internal.hpp).
#include <winsock2.h>

#include "MinHook.h"

#include <atomic>

#include "log.hpp"

namespace kcds::game_client {

namespace {

constexpr DWORD kHookBatchMilliseconds = 100;

std::atomic<DWORD> batching_thread{0};
std::atomic<bool> stop_applying{false};
HANDLE applier = nullptr;

void apply_queued() {
  const MH_STATUS status = MH_ApplyQueued();
  if (status != MH_OK) {
    log("HOOKS a batch was refused: %s", MH_StatusToString(status));
  }
}

DWORD WINAPI apply_batches(void*) {
  while (!stop_applying.load()) {
    Sleep(kHookBatchMilliseconds);
    apply_queued();
  }
  return 0;
}

}  // namespace

void begin_hook_batch() {
  applier = CreateThread(nullptr, 0, apply_batches, nullptr, 0, nullptr);
  // Without its thread nothing would apply a batch: the hooks stay immediate.
  if (applier) {
    batching_thread.store(GetCurrentThreadId());
  }
}

void end_hook_batch() {
  if (!applier) {
    return;
  }
  batching_thread.store(0);
  stop_applying.store(true);
  WaitForSingleObject(applier, INFINITE);
  CloseHandle(applier);
  applier = nullptr;
  apply_queued();
}

}  // namespace kcds::game_client

extern "C" MH_STATUS WINAPI kcds_enable_hook(LPVOID target) {
  const bool batched = kcds::game_client::batching_thread.load() == GetCurrentThreadId();
  if (!batched || target == MH_ALL_HOOKS) {
    return MH_EnableHook(target);
  }
  return MH_QueueEnableHook(target);
}
