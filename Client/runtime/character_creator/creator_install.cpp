// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "creator_install.hpp"

#include <cstdint>
#include <cstring>

#include "../chat/chat_overlay.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "creator_stage.hpp"

namespace kcds::game_client {

namespace native_chat = kcd2s::native_chat;

void install_character_creator() {
  // CharacterCreator: the pause screen's player page as the creator's stage.
  kcds::runtime::character_creator::Natives natives;
  natives.services = get_services;
  natives.framework = [] { return framework_slot ? *framework_slot : nullptr; };
  natives.entity_system = []() -> void* {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    return actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  };
  natives.spawn_params_init = resolve_unique(
      "EntitySpawnParams", "48 89 5C 24 08 57 48 83 EC 20 33 FF 48 8D 05 ?? ?? ?? ?? 48 89 41 28");
  natives.window = native_chat::window;
  // open_apse_player's handler reads the open context's global at + 62.
  if (auto open_player = static_cast<std::uint8_t*>(resolve_unique(
          "OpenApsePlayer",
          "4C 8B DC 49 89 5B 10 57 48 81 EC D0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 "
          "24 "
          "C0 00 00 00 48 8B 79 08 49 8D 53 98 48 8D 4C 24 20 48 8B 47 10 49 C7 43 D8 00 00 00 "
          "00 48 8B 58 08 48 8B 05 ?? ?? ?? ?? 49 89 43 98 E8 ?? ?? ?? ?? 4C 8B C0 48 8D 4F 10 "
          "B2 04"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, open_player + 65, 4);
    natives.open_context = open_player + 69 + displacement;
  }
  natives.stage_init = resolve_unique(
      "ApseCharacterStageInit",
      "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B CB 48 83 C4 20 5B E9 ?? ?? ?? ?? CC 4C 8B "
      "DC");
  // C_UIApsePlayer's constructor loads its vtable at + 25.
  if (auto player_page = static_cast<std::uint8_t*>(
          resolve_unique("ApsePlayerConstructor",
                         "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 56 48 83 "
                         "EC 20 48 8D 05 ?? "
                         "?? ?? ?? C6 41 10 01"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, player_page + 28, 4);
    natives.apse_player_vtable = player_page + 32 + displacement;
  }
  const bool creator = kcds::runtime::character_creator::install(natives, log);
  log("CREATOR stage %s", creator ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
