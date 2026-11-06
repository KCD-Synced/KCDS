// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "gathering_install.hpp"

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "herbs.hpp"

namespace kcds::game_client {

void install_gathering() {
  kcds::runtime::gathering::Natives natives;
  natives.services = [] { return get_services ? get_services() : nullptr; };
  natives.local_actor = [] { return local_combat_actor.load(); };
  // The respawn manager's pick, its respawn and the erase of one of its
  // pick records.
  natives.pick = resolve_unique(
      "HerbPick",
      "48 8B C4 48 89 58 08 48 89 70 10 48 89 78 18 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 81 "
      "EC A0 00 00 00 0F 29 70 C8 49");
  natives.respawn = resolve_unique(
      "HerbRespawn",
      "48 8B C4 48 89 58 08 48 89 70 18 48 89 78 20 55 41 54 41 57 48 8D 68 A1 48 81 EC B0");
  natives.erase_record =
      resolve_unique("HerbEraseRecord", "48 89 5C 24 08 57 48 83 EC 20 F3 0F 10 42 18 4C");
  // The herb gathering's grant of the herb.
  natives.grant = resolve_unique("HerbGrant", "48 89 5C 24 18 55 56 57 48 83 EC 40 48 8B 41");
  // The manager's gathering of the plants of a kind within a radius and the room of
  // its list, for map-wide picks.
  natives.collect = resolve_unique(
      "HerbCollect",
      "48 8B C4 48 89 58 20 55 56 57 41 54 41 55 41 56 41 57 48 8D 68 A9 48 81 EC B0 00 00 00 44 "
      "8B 05 ?? ?? ?? ?? 4C 8B E9 0F 29 70 B8 45 8A E1 0F 29 78 A8 0F 28 F2");
  natives.reserve = resolve_unique("HerbCollectReserve",
                                   "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 8B 3D ?? ?? ?? "
                                   "?? 48 8B F2 48 2B 3D ?? ?? ?? ?? 48 8B 0A 48 C1 FF 03");
  natives.log = log;
  log("GATHER %s", kcds::runtime::gathering::install(natives) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
