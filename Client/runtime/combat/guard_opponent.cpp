#include "guard_opponent.hpp"

#include "../core/natives.hpp"
#include "MinHook.h"

namespace kcds::runtime::combat::guard_opponent {
namespace {

// The guard's check of its opponent (guard action, opponent, attack kind): push rbx; sub rsp,
// 0x20; mov rax, [rdx + 0x1F8]; ... call [rax + 0x148]; cmp al, 1.
constexpr char kOpponentCheck[] =
    "40 53 48 83 EC 20 48 8B 82 F8 01 00 00 41 8B D8 48 8B 88 68 06 00 00 48 8B 01 FF 90 48 01 "
    "00 00 3C 01";

using OpponentCheck = bool(__fastcall*)(void* guard, void* opponent, unsigned kind);
OpponentCheck original_check = nullptr;

bool __fastcall hooked_check(void* guard, void* opponent, unsigned kind) {
  if (!opponent) {
    return false;
  }
  return original_check(guard, opponent, kind);
}

}  // namespace

bool install(Logger log) {
  void* check = kcds::game_client::resolve_unique("GuardOpponentCheck", kOpponentCheck);
  if (!check ||
      MH_CreateHook(check, reinterpret_cast<void*>(&hooked_check),
                    reinterpret_cast<void**>(&original_check)) != MH_OK ||
      MH_EnableHook(check) != MH_OK) {
    if (log) {
      log("COMBAT guard opponent check not hooked");
    }
    return false;
  }
  return true;
}

}  // namespace kcds::runtime::combat::guard_opponent
