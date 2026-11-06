#include "game_mods.hpp"

#include <cstdint>
#include <cstring>

#include "MinHook.h"

namespace kcds::runtime::game_mods {
namespace {

// mov rax,rsp ... sub rsp,0D0h; mov rax,[rcx+30h]; mov rbx,rcx; cmp rax,[rcx+38h].
constexpr const char* kFindModsSignature =
    "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 EC "
    "D0 00 00 00 48 8B 41 30 48 8B D9 48 3B 41 38 0F 85";
// The mod manager's lists: every mod found (begin, end; 112 bytes each) and the enabled ones
// (begin, end; a pointer to a found mod each).
constexpr std::size_t kFoundBegin = 24;
constexpr std::size_t kFoundEnd = 32;
constexpr std::size_t kFoundSize = 112;
constexpr std::size_t kEnabledBegin = 48;
constexpr std::size_t kEnabledEnd = 56;

using FindMods = char(__fastcall*)(void* manager);
FindMods original_find_mods = nullptr;
Logger log_line = nullptr;

std::uint8_t* pointer_at(void* object, std::size_t offset) {
  std::uint8_t* value = nullptr;
  std::memcpy(&value, static_cast<std::uint8_t*>(object) + offset, sizeof(value));
  return value;
}

char __fastcall hooked_find_mods(void* manager) {
  const char result = original_find_mods(manager);
  std::uint8_t* found_begin = pointer_at(manager, kFoundBegin);
  std::uint8_t* found_end = pointer_at(manager, kFoundEnd);
  std::uint8_t* enabled_begin = pointer_at(manager, kEnabledBegin);
  std::uint8_t* enabled_end = pointer_at(manager, kEnabledEnd);
  const std::size_t found = found_end >= found_begin ? (found_end - found_begin) / kFoundSize : 0;
  const std::size_t enabled =
      enabled_end >= enabled_begin ? (enabled_end - enabled_begin) / sizeof(void*) : 0;
  // The enabled list emptied as the game itself empties it (its end set to its begin).
  std::memcpy(static_cast<std::uint8_t*>(manager) + kEnabledEnd, &enabled_begin,
              sizeof(enabled_begin));
  if (log_line) {
    log_line("GAME MODS %zu found, %zu enabled by the game, all kept off for KCDS", found, enabled);
  }
  return result;
}

}  // namespace

bool install(Resolver resolve, Logger log) {
  log_line = log;
  void* find_mods = resolve("ModManagerFindMods", kFindModsSignature);
  const bool hooked = find_mods &&
                      MH_CreateHook(find_mods, reinterpret_cast<void*>(hooked_find_mods),
                                    reinterpret_cast<void**>(&original_find_mods)) == MH_OK &&
                      MH_EnableHook(find_mods) == MH_OK;
  if (log) {
    log("GAME MODS %s", hooked ? "guard installed" : "guard unavailable: the game's mods load");
  }
  return hooked;
}

}  // namespace kcds::runtime::game_mods
