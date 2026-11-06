#include "single_player_screens.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "kcd2s/bridge/sigscan.hpp"

#include <cstring>

namespace kcds::game_client {

namespace bridge = kcd2s::bridge;

StartSkipTime original_start_skip_time;
char __fastcall hooked_start_skip_time(void* self, unsigned mode, float a, float b,
                                       const void* state, int flags) {
  if (multiplayer_world.load())
    return 0;  // Before the native UI or action map is activated.
  return original_start_skip_time(self, mode, a, b, state, flags);
}
TutorialsEnabled original_tutorials_enabled;
// Set on the engine thread while the resources' Hud calls run: Hud.showTutorial is the one way
// a tutorial still shows in a multiplayer world.
bool resource_tutorial_call = false;
bool __fastcall hooked_tutorials_enabled() {
  if (resource_tutorial_call) {
    return true;
  }
  return !multiplayer_world.load() && original_tutorials_enabled();
}

// Native menu button filter (Client/menu_native owns the AddButton hook).
bool multiplayer_button_filter(std::uint8_t id) {
  // E_ButtonId: 11 SaveGame, 12 LoadGame, 19 SaveExit, 20 SaveReset.
  const bool save_or_load = id == 11 || id == 12 || id == 19 || id == 20;
  if (multiplayer_world.load() && save_or_load) {
    log("MULTIPLAYER menu suppressed save/load button=%u", unsigned(id));
    return true;
  }
  return false;
}

void* resolve_tutorial_query() {
  auto signature = bridge::Signature::parse(
      "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 48 8B 01 FF 90 B8 00 00 00 48 8B 08 "
      "48 8B 51 10 48 8B C8 FF D2 85 C0 0F 9F C0 48 83 C4 28 C3");
  if (!signature)
    return nullptr;
  void* result = nullptr;
  std::size_t cursor = 0;
  while (cursor < image_size) {
    auto offset = bridge::scan(image + cursor, image_size - cursor, *signature);
    if (offset == bridge::knpos)
      break;
    offset += cursor;
    std::int32_t displacement{};
    std::memcpy(&displacement, image + offset + 14, 4);
    const auto target = std::int64_t(offset) + 18 + displacement;
    constexpr char name[] = "wh_ui_TutorialsEnabled";
    if (target >= 0 && std::size_t(target) + sizeof(name) <= image_size &&
        !std::memcmp(image + target, name, sizeof(name))) {
      if (result) {
        log("Ambiguous tutorial query referenced literal");
        return nullptr;
      }
      result = image + offset;
    }
    cursor = offset + 1;
  }
  return result;
}

}  // namespace kcds::game_client
