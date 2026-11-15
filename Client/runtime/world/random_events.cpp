#include "random_events.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace kcds::runtime::world::random_events {
namespace {

// The variable is cheat protected (registered with VF_CHEAT): ICVar Set asks the console first and
// the console refuses. So the number itself is written. The game's int variables keep a pointer to
// it, and their GetIVal (ICVar slot 2) reads it: "mov rax, [rcx + offset]; mov eax, [rax]; ret",
// whose offset says where that pointer is. The variable is found with the console's GetCVar
// (slot 23) once a second: the game registers it again with each level.
constexpr char kAutoSpawn[] = "wh_pl_RandomEventsAutoSpawnEnabled";
constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarGetInt = 2;
constexpr ULONGLONG kCheckMs = 1000;
ULONGLONG next_check = 0;
bool unknown_logged = false;
bool unreadable_logged = false;

// The int an int variable keeps its value in, read from its GetIVal's code; null for another
// kind of variable.
int* variable_value(void* variable) {
  auto** table = *static_cast<void***>(variable);
  const auto* code = static_cast<const std::uint8_t*>(table[kCVarGetInt]);
  const bool reads_pointer = code[0] == 0x48 && code[1] == 0x8B && code[2] == 0x41 &&
                             code[4] == 0x8B && code[5] == 0x00 && code[6] == 0xC3;
  if (!reads_pointer) {
    return nullptr;
  }
  return *reinterpret_cast<int**>(static_cast<std::uint8_t*>(variable) + code[3]);
}

}  // namespace

void keep_off() {
  const ULONGLONG now = GetTickCount64();
  if (now < next_check) {
    return;
  }
  next_check = now + kCheckMs;
  void* console = kcds::game_client::console_slot ? *kcds::game_client::console_slot : nullptr;
  if (!console) {
    return;
  }
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  auto get = reinterpret_cast<GetCVar>((*static_cast<void***>(console))[kConsoleGetCVar]);
  void* variable = get(console, kAutoSpawn);
  if (!variable) {
    if (!unknown_logged) {
      kcds::game_client::log("RANDOM EVENTS %s unknown", kAutoSpawn);
    }
    unknown_logged = true;
    return;
  }
  int* value = variable_value(variable);
  if (!value) {
    if (!unreadable_logged) {
      kcds::game_client::log("RANDOM EVENTS %s is not an int variable this client can write",
                             kAutoSpawn);
    }
    unreadable_logged = true;
    return;
  }
  if (*value == 0) {
    return;
  }
  kcds::game_client::log("RANDOM EVENTS off (%s was %d)", kAutoSpawn, *value);
  *value = 0;
}

}  // namespace kcds::runtime::world::random_events
