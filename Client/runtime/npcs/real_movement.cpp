#include "../core/client_internal.hpp"

#include "real_movement.hpp"

#include <cstddef>
#include <cstdint>

#include "../core/natives.hpp"

namespace kcds::runtime::npcs::real_movement {
namespace {

// The game's fake movement request reads the MovementSystem global first:
// "mov rcx, [rip + X]" at + 0x2E, its operand at + 0x31, the instruction ending at + 0x35.
constexpr char kRequestPattern[] =
    "48 89 5C 24 18 48 89 74 24 20 55 57 41 56 48 8D 6C 24 B9 48 81 EC E0 00 00 00 48 8B 05 "
    "?? ?? ?? ?? 48 33 C4 48 89 45 37 48 8B F9 4C 8B F2 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 "
    "90 00 00 00";
constexpr int kSystemOperand = 0x31;
constexpr int kSystemInstructionEnd = 0x35;
// MovementSystem: its MovementActors (begin, end), each actor's entity id and fake flag.
constexpr std::size_t kActorsBegin = 80;
constexpr std::size_t kActorsEnd = 88;
constexpr std::size_t kActorBytes = 328;
constexpr std::size_t kActorEntity = 24;
constexpr std::size_t kActorFake = 313;
// MovementSystem's slot 25: (actor id, on) sets the flag; off resets the fake movement too.
constexpr std::size_t kSetFakeSlot = 25;

Logger log_line = nullptr;
void** system_global = nullptr;

}  // namespace

bool install(Logger logger) {
  log_line = logger;
  system_global = static_cast<void**>(kcds::game_client::resolve_unique(
      "MovementSystemGlobal", kRequestPattern, kSystemOperand, kSystemInstructionEnd));
  if (log_line) {
    log_line("NPC real movement: movement system global %s", system_global ? "found" : "missing");
  }
  return system_global != nullptr;
}

bool keep_real(std::uint32_t engine_id) {
  void* system = system_global ? *system_global : nullptr;
  if (!system || engine_id == 0) {
    return false;
  }
  auto* bytes = static_cast<std::uint8_t*>(system);
  auto* begin = *reinterpret_cast<std::uint8_t**>(bytes + kActorsBegin);
  auto* end = *reinterpret_cast<std::uint8_t**>(bytes + kActorsEnd);
  if (!begin || end < begin) {
    return false;
  }
  for (std::uint8_t* actor = begin; actor + kActorBytes <= end; actor += kActorBytes) {
    if (*reinterpret_cast<const std::uint32_t*>(actor + kActorEntity) != engine_id) {
      continue;
    }
    if (actor[kActorFake] == 0) {
      return false;
    }
    using SetFake = void(__fastcall*)(void* system, std::uint32_t entity, char on);
    auto set = reinterpret_cast<SetFake>((*static_cast<void***>(system))[kSetFakeSlot]);
    set(system, engine_id, 0);
    return true;
  }
  return false;
}

}  // namespace kcds::runtime::npcs::real_movement
