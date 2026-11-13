// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "body_state_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "body_readings.hpp"

namespace kcds::game_client {

void install_body_state() {
  // How the players' bodies stand and move (body_state).
  kcds::runtime::body_state::Natives natives;
  natives.animated_human =
      resolve_unique("ActorAnimatedHuman", "48 8B 81 68 02 00 00 C3 CC CC CC CC");
  natives.action_controller =
      resolve_unique("AnimatedCharacterActionController", "48 8B 81 80 00 00 00 C3 CC CC CC CC");
  natives.context =
      resolve_unique("ActionControllerContext", "48 8B 41 20 C3 CC CC CC ?? ?? ?? ?? ?? ?? 48 83");
  natives.physics = resolve_unique(
      "EntityPhysics", "40 53 48 83 EC 20 BA 01 00 00 00 48 8B D9 E8 ?? ?? ?? ?? 48 85 C0 74 17");
  natives.living_status = resolve_unique(
      "LivingEntityStatus",
      "48 8B C4 48 89 58 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 B8 48 81 EC 10 01 00 00 "
      "0F 29 70 B8 41 BE 01");
  natives.aspect_profile =
      resolve_unique("GameObjectAspectProfile", "4C 8B C1 48 8B 49 30 48 85 C9 74 1C");
  const bool body_state = kcds::runtime::body_state::install(natives, log);
  log("BODY STATE %s", body_state ? "installed" : "unavailable (posture and tags read defaults)");
}

}  // namespace kcds::game_client
