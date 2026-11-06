#include "pillory_hooks.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../doors/doors.hpp"
#include "../weapons/drawn_weapons.hpp"
#include "pillory.hpp"

namespace kcds::game_client {

void install_pillory_and_door_hooks() {
  // A player in the game's pillory, put there by the server: the player-state
  // handler's unstance calls and C_LockBase::SetLocked. Off, with this one line, when a
  // signature is missing.
  kcds::runtime::pillory::Natives pillory;
  pillory.handler = resolve_unique(
      "PlayerStateHandler",
      "40 53 48 83 EC 20 48 8B 1D ?? ?? ?? ?? 90 48 85 DB 75 7F 48 8D 0D ?? ?? ?? ?? E8 ?? ?? "
      "?? ?? 48 8B 1D ?? ?? ?? ?? 48 85 DB 75 5A 39 1D ?? ?? ?? ?? 75 05 E8 ?? ?? ?? ?? 48 8B "
      "05 ?? ?? ?? ?? 48 8D 54 24 30 BB D8 02 00");
  pillory.unstance_init =
      resolve_unique("UnstanceInit", "40 53 48 83 EC 20 C7 01 FF FF FF FF 48 8B D9 48");
  // An entity's WUID as the game's GetMyWUID binding gets it.
  pillory.wuid_lookup = resolve_unique(
      "WuidLookup",
      "48 89 54 24 10 53 48 83 EC 20 48 8B D9 48 8D 54 24 38 48 B9 25 23 22 84 E4 9C F2 CB 41 "
      "B8 08 00 00 00 E8 ?? ?? ?? ?? 4C 8B C0 48 8D 54 24 38 48 8B CB E8 ?? ?? ?? ?? 48 3B 43 "
      "08 75 0D 48 8D 05");
  pillory.wuid_map = resolve_unique("WuidMap",
                                    "48 8B 10 48 8B C8 48 8B 1D ?? ?? ?? ?? FF 52 10 48 8B D0 "
                                    "48 8B CB E8 ?? ?? ?? ?? 48 8B 08",
                                    9, 13);
  wuid_lookup_native = pillory.wuid_lookup;
  wuid_map_native = pillory.wuid_map;
  pillory.services = reinterpret_cast<void*>(get_services);
  pillory.unstance_by_name =
      resolve_unique("UnstanceByName", "48 89 4C 24 08 53 48 83 EC 20 49 89");
  pillory.unstance_apply = resolve_unique(
      "UnstanceApply",
      "40 55 53 56 57 41 54 41 56 41 57 48 8B EC 48 81 EC 80 00 00 00 48 8B 05 ?? ?? ?? ?? 48 "
      "33 C4 48 89 45 F8 48");
  pillory.set_locked = resolve_unique(
      "LockSetLocked", "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC 20 48 8B F1 49");
  lock_set_locked = pillory.set_locked;
  // The lock of an entity id, as the lock script bindings find it (the entity id at
  // + 0x28 of its argument, the game object of that id, then its lock component).
  lock_of_entity_id = resolve_unique(
      "LockOfEntityId",
      "40 53 48 83 EC 20 48 8B D9 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 80 00 00 00 48 8B C8 48 "
      "8B 10 4C 8B 82 50 02 00 00 8B 53 28 41 FF D0 48 8B D8 48");
  pillory.report_unlocked =
      resolve_unique("LockReportUnlocked",
                     "40 53 48 83 EC 20 48 8B DA E8 ?? ?? ?? ?? 48 85 C0 74 09 48 8D 48 50 E8");
  pillory.script_lock =
      resolve_unique("LockOfScript", "40 53 48 83 EC 20 48 8B 02 48 8B CA FF 50 10 48");
  pillory.holster = &kcds::runtime::weapons::holster;
  pillory.entity_system = []() -> void* {
    void* framework = framework_slot ? *framework_slot : nullptr;
    auto actors = framework
                      ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(framework) + 1304)
                      : nullptr;
    return actors ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(actors) + 48) : nullptr;
  };
  log("PILLORY %s", kcds::runtime::pillory::install(pillory, log) ? "installed" : "unavailable");
  // The level's doors: the door script class AnimDoor's native side. OpenDoor and
  // the CloseDoor body are the game's own script-called natives (wh::entitymodule::OpenDoor
  // and CloseDoor), SetLocked is the pillory's lock call, and the lock of an entity id is the
  // lookup the lock script bindings make.
  kcds::runtime::doors::Natives doors;
  doors.open_door = resolve_unique(
      "DoorOpen",
      "48 85 C9 74 5B 88 54 24 10 53 48 83 EC 60 48 8B D9 E8 ?? ?? ?? ?? 84 C0 75 41 48 8D 05");
  doors.close_door = resolve_unique(
      "DoorClose",
      "48 85 C9 74 60 88 54 24 10 53 48 83 EC 60 48 8B D9 45 84 C0 74 09 E8 ?? ?? ?? ?? 84 C0 "
      "75 41 48 8D 05");
  doors.set_locked = pillory.set_locked;
  doors.lock_of_entity_id = lock_of_entity_id;
  doors.entity_system = pillory.entity_system;
  doors.send_report = [](const std::vector<kcd2s::integration::DoorEntry>& list) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_door_report(list);
  };
  doors.send_use = [](const kcd2s::integration::DoorUse& use) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_door_use(use);
  };
  doors.send_reply = [](const kcd2s::integration::DoorNearestReply& reply) {
    std::lock_guard<std::mutex> lock(network_mutex);
    return client && client->peer().send_door_nearest_reply(reply);
  };
  log("DOORS %s", kcds::runtime::doors::install(doors, log) ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
