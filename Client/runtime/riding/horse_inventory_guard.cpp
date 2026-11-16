#include "horse_inventory_guard.hpp"

#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::riding::horse_inventory_guard {
namespace {

using FreeCapacity = float(__fastcall*)(void* helper);
using Companion = void*(__fastcall*)(void* manager, char index);

// The helper's owner at + 8, the companion manager at + 0x90 of it, and the call of the
// companion getter (E8 rel32) at + 0x18 of the helper.
constexpr std::size_t kOwner = 8;
constexpr std::size_t kCompanionManager = 0x90;
constexpr std::size_t kCompanionCall = 0x18;

FreeCapacity original = nullptr;
Companion companion = nullptr;
Logger log_line = nullptr;
bool logged = false;

float __fastcall hooked(void* helper) {
  auto* owner = helper
                    ? *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(helper) + kOwner)
                    : nullptr;
  void* manager = owner ? *reinterpret_cast<void**>(owner + kCompanionManager) : nullptr;
  if (!manager || !companion(manager, 0)) {
    if (log_line && !logged) {
      logged = true;
      log_line("HORSE inventory: no companion to carry anything, nothing fits (logged once)");
    }
    return 0.0f;
  }
  return original(helper);
}

}  // namespace

bool install(void* free_capacity, Logger logger) {
  log_line = logger;
  if (!free_capacity) {
    return false;
  }
  auto* call = static_cast<std::uint8_t*>(free_capacity) + kCompanionCall;
  if (call[0] != 0xE8) {
    return false;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, call + 1, sizeof(relative));
  companion = reinterpret_cast<Companion>(call + 5 + relative);
  return MH_CreateHook(free_capacity, reinterpret_cast<void*>(&hooked),
                       reinterpret_cast<void**>(&original)) == MH_OK &&
         MH_EnableHook(free_capacity) == MH_OK;
}

}  // namespace kcds::runtime::riding::horse_inventory_guard
