#include "../core/client_internal.hpp"

#include "nav_listener_probe.hpp"

#include <cstdint>
#include <map>
#include <set>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::testing::nav_listeners {
namespace {

using kcds::game_client::log;
using kcds::game_client::resolve_unique;

// The navigation module's update (its vtable slot 2: the module, a time, a flag,
// a count) and the listener set it notifies after tile streaming (reads the module's
// + 0x158; the set keeps its listeners in a vector at + 8 to + 16).
constexpr std::size_t kListenerSet = 0x158;
constexpr std::size_t kVectorBegin = 8;
constexpr std::size_t kVectorEnd = 16;
constexpr std::uint64_t kCheckMs = 0;

using Update = std::int64_t(__fastcall*)(void* module, void* time, char flag, unsigned count);
Update original_update = nullptr;

std::set<void*> known;
// Each listener's vtable when it was added: a listener whose memory was freed and reused shows
// another one, which is the state the crash was in.
std::map<void*, void*> tables;
std::uint64_t checked_at = 0;

void check(void* module) {
  void* listeners = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(module) + kListenerSet);
  if (!listeners) {
    return;
  }
  auto* bytes = static_cast<std::uint8_t*>(listeners);
  auto* begin = *reinterpret_cast<void***>(bytes + kVectorBegin);
  auto* end = *reinterpret_cast<void***>(bytes + kVectorEnd);
  if (!begin || end < begin || end - begin > 4096) {
    return;
  }
  std::set<void*> now(begin, end);
  for (void* listener : now) {
    void* table = listener ? *static_cast<void**>(listener) : nullptr;
    if (known.count(listener) == 0) {
      tables[listener] = table;
      log("NAVPROBE listener added %p vtable=%p (size %zu)", listener, table, now.size());
    } else if (tables[listener] != table) {
      log("NAVPROBE STALE listener %p vtable=%p, was %p (size %zu)", listener, table,
          tables[listener], now.size());
      tables[listener] = table;
    }
  }
  for (void* listener : known) {
    if (now.count(listener) == 0) {
      tables.erase(listener);
      log("NAVPROBE listener removed %p (size %zu)", listener, now.size());
    }
  }
  known = std::move(now);
}

std::int64_t __fastcall probed_update(void* module, void* time, char flag, unsigned count) {
  const std::uint64_t now = GetTickCount64();
  if (now - checked_at >= kCheckMs) {
    checked_at = now;
    check(module);
  }
  return original_update(module, time, flag, count);
}

}  // namespace

bool install() {
  void* update = resolve_unique(
      "NavModuleUpdate",
      "48 8B C4 48 89 58 08 48 89 70 10 57 48 83 EC 30 41 8B D9 48 8B F1 45 84 C0 75 4E 48 C7 40 "
      "E8 00 00 00 00 C7 40 F0 00 00 00 00 E8");
  if (!update ||
      MH_CreateHook(update, reinterpret_cast<void*>(probed_update),
                    reinterpret_cast<void**>(&original_update)) != MH_OK ||
      MH_EnableHook(update) != MH_OK) {
    log("NAVPROBE unavailable");
    return false;
  }
  log("NAVPROBE installed");
  return true;
}

}  // namespace kcds::runtime::testing::nav_listeners
