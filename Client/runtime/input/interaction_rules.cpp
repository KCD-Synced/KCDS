#include "interaction_rules.hpp"

#include <cstring>

#include "kcd2s/integration/interaction_rules.hpp"

namespace kcds::runtime::input::interaction_rules {
namespace {

// The companion key's whistle (Libs/Config/keybindSuperactions.xml companion_interaction): the
// call action map's "call" and the quest map's own call of the horse. The key's other actions
// (mounting, dismounting, the cart, the dog) are not the whistle and go through.
constexpr const char* kWhistleActions[] = {"call", "custom_quest_call_horse_action"};
constexpr unsigned kReleased = 2;

std::uint16_t allowed_bits = 0;

}  // namespace

void set_allowed(std::uint16_t bits, void* console, bool in_world) {
  allowed_bits = bits;
  if (!console) {
    return;
  }
  // IConsole::GetCVar (slot 23) and ICVar::Set(int) (slot 7), as the client sets its other cvars.
  using GetCVar = void*(__fastcall*)(void* console, const char* name);
  using SetInt = void(__fastcall*)(void* var, int value);
  void* var = (*static_cast<GetCVar**>(console))[23](console, "wh_dlg_Enable");
  if (!var) {
    return;
  }
  const bool talk = !in_world || !allowed(kcd2s::integration::kRefuseNpcChatter);
  (*static_cast<SetInt**>(var))[7](var, talk ? 1 : 0);
}

bool allowed(std::uint16_t bit) {
  return (allowed_bits & bit) != 0;
}

bool refuse_action(const char* action, unsigned mode) {
  if (!action || mode == kReleased || allowed(kcd2s::integration::kAllowHorseWhistle)) {
    return false;
  }
  for (const char* whistle : kWhistleActions) {
    if (std::strcmp(action, whistle) == 0) {
      return true;
    }
  }
  return false;
}

}  // namespace kcds::runtime::input::interaction_rules
