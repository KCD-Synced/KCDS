// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "knockout_install.hpp"

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../needs/needs.hpp"
#include "../pillory/pillory_hooks.hpp"
#include "../world/world_entity_hooks.hpp"
#include "knockout_hooks.hpp"

namespace kcds::game_client {

DWORD install_knockout_and_world_hooks() {
  const auto knockout_natives = resolve_knockout_natives();
  install_pillory_and_door_hooks();
  install_world_entity_hooks();
  // The game's sleep and food timers stopped while the server has the needs off.
  kcds::runtime::needs::install(console_slot, log);
  if (!install_knockout(knockout_natives)) {
    return 1;
  }
  return 0;
}

}  // namespace kcds::game_client
