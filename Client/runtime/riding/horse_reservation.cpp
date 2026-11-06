#include "horse_reservation.hpp"

#include <atomic>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"

namespace kcds::runtime::riding {
namespace {

using Assignment = std::uintptr_t(__fastcall*)(void* assignments, const std::uint64_t* horse);
using FindActor = void*(__fastcall*)(void* registry, const std::uint64_t* id);

// The actor registry's find by id.
constexpr std::size_t kFindActor = 3;

Logger log_line = nullptr;
void** registry = nullptr;
Assignment original_reserve = nullptr;
Assignment original_release = nullptr;
std::atomic<unsigned> skipped{0};

bool horse_found(const std::uint64_t* horse) {
  void* actors = registry ? *registry : nullptr;
  if (!actors || !horse) {
    return false;
  }
  auto find = (*reinterpret_cast<FindActor**>(actors))[kFindActor];
  return find(actors, horse) != nullptr;
}

void report_skip(const char* what, const std::uint64_t* horse) {
  const unsigned count = ++skipped;
  // The first one and then every hundredth, so the log shows it without filling up.
  if (log_line && (count == 1 || count % 100 == 0)) {
    log_line("RIDING horse %llx %s skipped: the game no longer finds it (%u so far)",
             horse ? static_cast<unsigned long long>(*horse) : 0ULL, what, count);
  }
}

std::uintptr_t __fastcall reserve(void* assignments, const std::uint64_t* horse) {
  if (!horse_found(horse)) {
    report_skip("reservation", horse);
    return 0;
  }
  return original_reserve(assignments, horse);
}

std::uintptr_t __fastcall release(void* assignments, const std::uint64_t* horse) {
  if (!horse_found(horse)) {
    report_skip("release", horse);
    return 0;
  }
  return original_release(assignments, horse);
}

bool hook(void* target, void* detour, Assignment* original) {
  return target && MH_CreateHook(target, detour, reinterpret_cast<void**>(original)) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool guard_horse_reservations(const HorseReservationNatives& natives, Logger logger) {
  log_line = logger;
  registry = natives.registry;
  if (!registry) {
    return false;
  }
  return hook(natives.reserve, reinterpret_cast<void*>(reserve), &original_reserve) &&
         hook(natives.release, reinterpret_cast<void*>(release), &original_release);
}

}  // namespace kcds::runtime::riding
