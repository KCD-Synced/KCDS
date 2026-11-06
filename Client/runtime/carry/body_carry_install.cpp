// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "body_carry_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "body_carry.hpp"

namespace kcds::game_client {

void install_body_carry() {
  // Players' bodies carried on the shoulder.
  log("CARRY bodies %s",
      kcds::runtime::carry::install_body_carry(
          resolve_unique("CorpseCarry",
                         "40 53 48 83 EC 20 48 83 B9 88 01 00 00 00 48 8B D9 75 3C "
                         "48 8B D1 48 8D 4C 24 30 E8"),
          log)
          ? "installed"
          : "unavailable");
}

}  // namespace kcds::game_client
