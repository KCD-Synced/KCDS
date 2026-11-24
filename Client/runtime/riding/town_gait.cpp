#include "town_gait.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "MinHook.h"

namespace kcds::runtime::riding::town_gait {
namespace {

// The controller's rider (+ 16), the rider's town state (+ 3432) and its "in a town" byte (+ 56).
constexpr std::size_t kControllerRider = 16;
constexpr std::size_t kRiderTownState = 3432;
constexpr std::size_t kTownStateInTown = 56;

using BuildCommand = std::int64_t(__fastcall*)(void* controller, void* command, void* rider_input);

BuildCommand original_build = nullptr;
Logger log_line = nullptr;
std::atomic<bool> limited{true};

std::uint8_t* in_town_flag(void* controller) {
  auto* rider =
      *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(controller) + kControllerRider);
  auto* state = rider ? *reinterpret_cast<std::uint8_t**>(rider + kRiderTownState) : nullptr;
  return state ? state + kTownStateInTown : nullptr;
}

std::int64_t __fastcall hooked_build(void* controller, void* command, void* rider_input) {
  std::uint8_t* flag = !limited.load() && controller ? in_town_flag(controller) : nullptr;
  if (!flag || *flag == 0) {
    return original_build(controller, command, rider_input);
  }
  const std::uint8_t was = *flag;
  *flag = 0;
  const std::int64_t result = original_build(controller, command, rider_input);
  *flag = was;
  static int told = 0;
  if (log_line && told < 3) {
    ++told;
    log_line(
        "RIDING the town gait limit lifted for the rider's command (server horseTownLimit off)");
  }
  return result;
}

}  // namespace

bool install(void* command_builder, Logger logger) {
  log_line = logger;
  return command_builder &&
         MH_CreateHook(command_builder, reinterpret_cast<void*>(&hooked_build),
                       reinterpret_cast<void**>(&original_build)) == MH_OK &&
         MH_EnableHook(command_builder) == MH_OK;
}

void set_limited(bool value) {
  limited.store(value);
}

}  // namespace kcds::runtime::riding::town_gait
