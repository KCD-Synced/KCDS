// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "aim_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "aim.hpp"

namespace kcds::game_client {

void install_aim() {
  kcds::runtime::ranged::AimNatives aim;
  aim.set_target = resolve_unique(
      "PoseBlenderAimTarget",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D 72 0C 48 8B FA 48 8B "
      "E9 48 8B DA 48 3B D6 74 16 F3 0F 10 03 E8 4D F7 77 FF");
  log("AIM %s", kcds::runtime::ranged::install_aim(aim, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
