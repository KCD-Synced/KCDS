// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "interior_light.hpp"

#include <array>
#include <cstddef>

#include "../core/log.hpp"
#include "kcd1_map.hpp"

namespace kcds::runtime::kcd1::interior_light {
namespace {

// The engine console's GetCVar and a variable's GetIVal and Set(int), as the console uses them.
constexpr std::size_t kConsoleGetCVar = 23;
constexpr std::size_t kCVarGetInt = 2;
constexpr std::size_t kCVarSetInt = 7;
using GetCVar = void*(__fastcall*)(void* console, const char* name);
using CVarGetInt = int(__fastcall*)(void* cvar);
using CVarSetInt = void(__fastcall*)(void* cvar, int value);

template <class Fn>
Fn slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

struct Setting {
  const char* name;
  int kcd1_value;
  int game_value;
};

// The order matters: the voxels come from the geometry before the tree is (re)built.
std::array<Setting, 3> settings = {{
    {"e_svoStreamVoxels", 0, 1},
    {"e_svoTI_UpdateGeometry", 1, 0},
    {"e_svoLoadTree", 1, 0},
}};
bool switched = false;

void* cvar(const char* name) {
  void* console = game_client::console_slot ? *game_client::console_slot : nullptr;
  return console ? slot<GetCVar>(console, kConsoleGetCVar)(console, name) : nullptr;
}

}  // namespace

void apply(const std::string& map) {
  const bool kcd1 = is_level(map);
  if (kcd1 == switched) {
    return;
  }
  for (auto& setting : settings) {
    void* variable = cvar(setting.name);
    if (!variable) {
      game_client::log("KCD1 interior light: %s unknown", setting.name);
      continue;
    }
    if (kcd1) {
      // Keeps the game's own value for the way back.
      setting.game_value = slot<CVarGetInt>(variable, kCVarGetInt)(variable);
    }
    slot<CVarSetInt>(variable, kCVarSetInt)(variable,
                                            kcd1 ? setting.kcd1_value : setting.game_value);
  }
  switched = kcd1;
  game_client::log("KCD1 interior light: global illumination %s",
                   kcd1 ? "built from the level" : "from the level's files");
}

}  // namespace kcds::runtime::kcd1::interior_light
