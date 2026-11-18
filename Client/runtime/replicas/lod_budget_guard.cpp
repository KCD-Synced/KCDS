#include "lod_budget_guard.hpp"

#include <cstddef>
#include <cstdint>

#include "MinHook.h"
#include "lod_budget_queue.hpp"

namespace kcds::runtime::replicas {
namespace {

using BudgetUpdate = std::int64_t(__fastcall*)(void* budget);

// C_ActorMonsterLODBudgetting: the raising list (begin, end), the lowering list (begin, end), the
// queue (items, count) and the flag that has the update take every waiting actor instead.
constexpr std::size_t kRaiseBegin = 0x08;
constexpr std::size_t kRaiseEnd = 0x10;
constexpr std::size_t kLowerBegin = 0x20;
constexpr std::size_t kLowerEnd = 0x28;
constexpr std::size_t kQueueItems = 0x38;
constexpr std::size_t kQueueCount = 0x40;
constexpr std::size_t kTakeAll = 0x64;

BudgetUpdate original_update = nullptr;
BudgetLogger write_log = nullptr;
unsigned logged = 0;
constexpr unsigned kMaxLogged = 8;

template <class T>
T& field(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

std::int64_t __fastcall guarded_update(void* budget) {
  // With the flag set the update empties both lists and the queue without reading it.
  if (!field<std::uint8_t>(budget, kTakeAll)) {
    void** queue = field<void**>(budget, kQueueItems);
    auto& count = field<std::uint64_t>(budget, kQueueCount);
    if (queue && count) {
      const std::size_t kept =
          keep_takeable(queue, static_cast<std::size_t>(count), field<void**>(budget, kRaiseBegin),
                        field<void**>(budget, kRaiseEnd), field<void**>(budget, kLowerBegin),
                        field<void**>(budget, kLowerEnd));
      if (kept != count) {
        if (write_log && logged < kMaxLogged) {
          ++logged;
          write_log("REPLICA LOD budget: %llu queued actors left out (queued twice or in no list)",
                    static_cast<unsigned long long>(count - kept));
        }
        count = kept;
      }
    }
  }
  return original_update(budget);
}

}  // namespace

bool install_lod_budget_guard(void* budget_update, BudgetLogger logger) {
  write_log = logger;
  if (!budget_update) {
    return false;
  }
  return MH_CreateHook(budget_update, reinterpret_cast<void*>(guarded_update),
                       reinterpret_cast<void**>(&original_update)) == MH_OK &&
         MH_EnableHook(budget_update) == MH_OK;
}

}  // namespace kcds::runtime::replicas
