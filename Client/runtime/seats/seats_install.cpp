// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "seats_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../props/prop_entity.hpp"
#include "seat_collisions.hpp"
#include "seat_reservations.hpp"

namespace kcds::game_client {

void install_seats() {
  // Seats: a seat a player sits on is taken on every client.
  kcds::runtime::seats::Natives natives;
  natives.reserve = resolve_unique("SmartObjectReserve",
                                   "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41 0F B7 D8");
  natives.release =
      resolve_unique("SmartObjectRelease", "4C 8B DC 49 89 5B 20 55 56 57 48 83 EC 20");
  natives.holds = resolve_unique("SmartObjectHolds", "4C 8B DC 49 89 5B 18 49 89 53 10 57");
  natives.find_smart_object =
      resolve_unique("SmartObjectByWuid", "48 89 54 24 10 48 83 EC 28 48 8B C2 48 C1 E8 38");
  natives.find_npc = resolve_unique("NpcManagerFind",
                                    "48 89 5C 24 08 48 89 7C 24 10 4C 8B 51 08 49 8B D8 4C 8B 09 "
                                    "4C 8B DA 4D 85 D2 0F 84");
  // `mov r8, [rip + disp32]` of the NPC manager in a getter of it.
  if (auto getter = static_cast<std::uint8_t*>(
          resolve_unique("NpcManagerGetter", "4C 8B 05 ?? ?? ?? ?? 48 8D 51 38 49 8B C8"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, getter + 3, 4);
    natives.npc_manager = reinterpret_cast<void* const*>(getter + 7 + displacement);
  }
  const bool seats = kcds::runtime::seats::install(natives, log);
  log("SEATS %s", seats ? "installed" : "unavailable");
  // Seated players do not push or block each other's bodies.
  kcds::runtime::seats::collisions::Natives collisions;
  collisions.physics_component = resolve_unique(
      "EntityPhysicsComponent",
      "48 89 5C 24 10 55 56 57 41 56 41 57 48 8B EC 48 83 EC 40 48 8D 99 A8 00 00 00");
  collisions.entity_by_id = &kcds::runtime::props::entity::by_id;
  const bool passing = kcds::runtime::seats::collisions::install(collisions, log);
  log("SEATS seated bodies %s", passing ? "pass through" : "keep their physics");
}

}  // namespace kcds::game_client
