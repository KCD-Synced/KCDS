#include "replica_faint.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

namespace kcds::runtime::knockout::replica_faint {
namespace {

// The check takes the buff instance and leaves a float in xmm0, which its caller does not use.
using ExhaustedCheck = double(__fastcall*)(void* instance);
// The instance's soul (C_BuffInstance + 136), and the soul's flags the check tests.
constexpr std::size_t kInstanceSoul = 136;
constexpr std::size_t kSoulFlags = 0xCF0;
constexpr std::uint32_t kPlayerFlag = 0x10;

ExhaustedCheck original_check = nullptr;
IsReplicaSoul replica_soul = nullptr;
Logger log_line = nullptr;
std::atomic<bool> skip_logged{false};

double __fastcall hooked_check(void* instance) {
  void* soul = nullptr;
  if (instance) {
    std::memcpy(&soul, static_cast<const std::uint8_t*>(instance) + kInstanceSoul, sizeof(soul));
  }
  if (!soul || !replica_soul(soul)) {
    return original_check(instance);
  }
  if (log_line && !skip_logged.exchange(true)) {
    std::uint32_t flags = 0;
    std::memcpy(&flags, static_cast<const std::uint8_t*>(soul) + kSoulFlags, sizeof(flags));
    log_line("KNOCKOUT replica soul=%p exhaustion faint check skipped (player flag=%u)", soul,
             unsigned((flags & kPlayerFlag) != 0));
  }
  return 0.0;
}

}  // namespace

bool install(void* exhausted_check, IsReplicaSoul is_replica_soul, Logger logger) {
  log_line = logger;
  replica_soul = is_replica_soul;
  if (!exhausted_check || !is_replica_soul) {
    return false;
  }
  return MH_CreateHook(exhausted_check, reinterpret_cast<void*>(hooked_check),
                       reinterpret_cast<void**>(&original_check)) == MH_OK &&
         MH_EnableHook(exhausted_check) == MH_OK;
}

}  // namespace kcds::runtime::knockout::replica_faint
