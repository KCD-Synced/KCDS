#include "movement_listener_guard.hpp"

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "MinHook.h"
#include "movement_listeners.hpp"

namespace kcds::runtime::npcs {
namespace {

using Notify = std::int64_t(__fastcall*)(void* owner);

// The owner's listener list at + 0x158; the list keeps its listeners in a vector at + 8 to + 16.
constexpr std::size_t kListenerList = 0x158;
constexpr std::size_t kVectorBegin = 0x08;
constexpr std::size_t kVectorEnd = 0x10;
// A list longer than this is no listener list of the navigation module's.
constexpr std::size_t kMostListeners = 4096;

Notify original_notify = nullptr;
const void* movement_task_vtable = nullptr;
ListenerLogger write_log = nullptr;
std::atomic<unsigned> logged{0};
constexpr unsigned kMaxLogged = 8;

template <class T>
T& field(void* object, std::size_t offset) {
  return *reinterpret_cast<T*>(static_cast<std::uint8_t*>(object) + offset);
}

// The object's vtable, or nullptr when its first 8 bytes are not readable memory (a freed task's
// page may be gone).
const void* readable_vtable(void* object) {
  MEMORY_BASIC_INFORMATION info{};
  if (!object || VirtualQuery(object, &info, sizeof(info)) != sizeof(info)) {
    return nullptr;
  }
  constexpr DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                              PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  if (info.State != MEM_COMMIT || (info.Protect & kReadable) == 0 ||
      (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
    return nullptr;
  }
  const auto end = static_cast<std::uint8_t*>(info.BaseAddress) + info.RegionSize;
  if (static_cast<std::uint8_t*>(object) + sizeof(void*) > end) {
    return nullptr;
  }
  return *static_cast<const void* const*>(object);
}

std::int64_t __fastcall guarded_notify(void* owner) {
  void* list = owner ? field<void*>(owner, kListenerList) : nullptr;
  if (list) {
    void** begin = field<void**>(list, kVectorBegin);
    void**& end = field<void**>(list, kVectorEnd);
    if (begin && end > begin && std::size_t(end - begin) <= kMostListeners) {
      const std::size_t count = std::size_t(end - begin);
      const std::size_t kept = keep_live_tasks(begin, count, movement_task_vtable, readable_vtable);
      if (kept != count) {
        end = begin + kept;
        if (write_log && logged.fetch_add(1) < kMaxLogged) {
          write_log("NPC NAV %llu freed movement task(s) taken out of the tile listeners",
                    static_cast<unsigned long long>(count - kept));
        }
      }
    }
  }
  return original_notify(owner);
}

}  // namespace

bool install_movement_listener_guard(void* notify, const void* task_vtable, ListenerLogger logger) {
  write_log = logger;
  movement_task_vtable = task_vtable;
  if (!notify || !task_vtable) {
    return false;
  }
  return MH_CreateHook(notify, reinterpret_cast<void*>(guarded_notify),
                       reinterpret_cast<void**>(&original_notify)) == MH_OK &&
         MH_EnableHook(notify) == MH_OK;
}

}  // namespace kcds::runtime::npcs
