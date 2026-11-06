// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "movement_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "movement_mode.hpp"
#include "shared_ladders.hpp"
#include "walk_default.hpp"

namespace kcds::game_client {

DWORD install_movement() {
  kcds::runtime::movement::Natives movement_natives;
  // The actor's vtable slot 222, SetMovementRestriction.
  movement_natives.restriction = resolve_unique(
      "ActorMovementRestriction",
      "48 89 5C 24 20 55 56 57 48 81 EC A0 02 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 84 24 "
      "90 02 00 00 44 88 81 09 04 00 00 48 8B D9 88 91 0A 04 00 00");
  kcds::runtime::movement::walk_default::install(log);
  // the grabs of a ladder, whether one may be used
  // (movement/shared_ladders.hpp).
  if (!kcds::runtime::movement::ladders::install(
          resolve_unique("LadderHelperGrab",
                         "48 8B C4 48 89 58 08 48 89 78 10 55 48 8D 68 D8 48 81 EC 20 01 00 00 "
                         "0F 29 70 E8 48 8B DA 0F 29 78 D8 4C 8B C2"),
          resolve_unique("GrabOnLadder",
                         "48 89 5C 24 20 57 48 83 EC 20 48 8B D9 89 54 24 38 48 8B 49 08 8B FA "
                         "48 8B 01 FF 50 10 48 8B C8 E8"),
          resolve_unique("CanUseLadder",
                         "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 56 41 57 48 8D 68 "
                         "A9 48 81 EC C0 00 00 00 0F 29 70 D8 48 8B F1 48 8B 49 38 48 8B DA 0F "
                         "29 78"),
          [](std::uint32_t id, float* matrix) {
            void* framework = framework_slot ? *framework_slot : nullptr;
            auto actors =
                framework ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                          : nullptr;
            void* entities =
                actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48)
                       : nullptr;
            void* entity = entities && id ? get_entity(entities, id) : nullptr;
            const float* tm =
                entity ? slot<const float*(__fastcall*)(void*)>(entity, 32)(entity) : nullptr;
            if (!tm) {
              return false;
            }
            std::memcpy(matrix, tm, 12 * sizeof(float));
            return true;
          },
          log)) {
    log("LADDER grab native missing; two players could climb one ladder through each other");
  }
  if (!kcds::runtime::movement::install(movement_natives, log)) {
    log("MOVEMENT restriction native missing; Player.setPace would do nothing");
    return 1;
  }
  return 0;
}

}  // namespace kcds::game_client
