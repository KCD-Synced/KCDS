// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "world_map_install.hpp"

#include <cstdint>
#include <cstring>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../kcd1/map_screen.hpp"
#include "../markers/marker_hooks.hpp"
#include "kcd2s/bridge/sigscan.hpp"
#include "map_discover.hpp"
#include "resource_world_map.hpp"
#include "travel_check.hpp"

namespace kcds::game_client {

namespace {

// The KCD1 map's world map on the game's map screen (kcd1/map_screen.hpp).
void install_kcd1_map_screen() {
  kcds::runtime::kcd1::map_screen::Natives natives;
  // The level manager's record of a level by id (a lower bound over the records, 152 bytes each).
  natives.level_record =
      resolve_unique("LevelRecordById",
                     "48 89 7C 24 08 4C 8B 05 ?? ?? ?? ?? 48 B8 1B CA 6B 28 AF A1 BC 86 48 8B 0D");
  natives.string_assign = string_assign;
  // std::vector<int>'s assignment: ScriptBind_Dice's next-throw override calls it at + 0xF0.
  constexpr std::size_t kOverrideAssignCall = 0xF0;
  if (auto* override_faces = static_cast<std::uint8_t*>(resolve_unique(
          "DiceThrowOverride",
          "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 A8 48 81 EC 58 01 00 00 48 8B 05 ?? "
          "?? ?? ?? 48 33 C4 48 89 45 40 4D"))) {
    const std::uint8_t* call = override_faces + kOverrideAssignCall;
    if (call[0] == 0xE8) {
      std::int32_t relative = 0;
      std::memcpy(&relative, call + 1, sizeof(relative));
      natives.assign_ints = reinterpret_cast<kcds::runtime::kcd1::map_screen::AssignInts>(
          const_cast<std::uint8_t*>(call + 5 + relative));
    }
  }
  natives.map_label = resolve_unique(
      "MapLabelFromRecord",
      "48 89 5C 24 18 55 56 57 48 83 EC 40 48 8B 42 28 33 FF 48 8B EA 48 8B F1 48 85 C0 0F 84 ?? "
      "?? ?? ?? 40 38 38 0F 84");
  natives.log = log;
  kcds::runtime::kcd1::map_screen::install(natives);
  log("KCD1 map screen %s",
      natives.level_record && natives.assign_ints ? "installed" : "unavailable");
}

}  // namespace

void install_world_map() {
  install_kcd1_map_screen();
  // WorldMap: the player's map marker, the game's checkpoint.
  kcds::runtime::world_map::Natives natives;
  natives.services = get_services;
  // UIMap.GoToCheckpointMark calls the map module getter at +39 and reads the
  // map at module + 0x1508.
  if (auto go_to = static_cast<std::uint8_t*>(
          resolve_unique("GoToCheckpointMark",
                         "48 8B C4 48 89 58 08 48 89 70 18 57 48 83 EC 60 0F 29 70 E8 48 8B FA "
                         "0F 29 78 D8 E8 ?? "
                         "?? ?? ?? 48 8B 88 E8 00 00 00 E8 ?? ?? ?? ?? 48 8D 54 24 78 48 8D 88 "
                         "08 15 00 00 E8"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, go_to + 40, 4);
    natives.module_getter = go_to + 44 + displacement;
  }
  natives.set_checkpoint = resolve_unique(
      "SetCheckpoint",
      "40 55 53 57 48 8B EC 48 81 EC 80 00 00 00 80 B9 08 06 00 00 00 48 8B F9 0F 29 74 24 70 "
      "0F 29 7C 24 60 44 0F 29 44 24 50 0F 84");
  natives.remove_checkpoint = resolve_unique(
      "RemoveCheckpoint",
      "40 53 48 83 EC 40 48 8D 54 24 20 48 8B D9 E8 ?? ?? ?? ?? 48 83 7C 24 20 00 74 ?? 48 8B 4B "
      "08 E8 ?? ?? ?? ?? 48 8D 54 24 20 4C 8B C0 48 8D 4C 24 30 E8");
  natives.refresh_hints = resolve_unique(
      "CheckpointHints",
      "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 80 B9 48 07 00 00 00 48 8B D9 0F 84 ?? ?? ?? "
      "?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 90 18 01 00 00");
  natives.redraw = resolve_unique("MapRedraw",
                                  "48 89 5C 24 08 55 56 57 48 8B EC 48 83 EC 40 40 8A F2 48 8B D9 "
                                  "E8 ?? ?? ?? ?? 48 8B CB 4C 8B 80 28 01 00 00 49 8B 78 60 E8");
  natives.compass_add = resolve_unique(
      "CompassMarkAdd",
      "40 55 53 56 57 48 8D 6C 24 C1 48 81 EC 88 00 00 00 48 8B D9 48 89 51 38 E8 ?? ?? ?? ?? E8 "
      "?? ?? ?? ?? 48 8B 48 18 48 8B 01 FF 90 18 01 00 00");
  natives.compass_remove = resolve_unique(
      "CompassMarkRemove",
      "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 20 48 8B 59 38 48 8D 15 ?? ?? ?? ?? 48 8B F9 48 "
      "8D 4C 24 38 E8 ?? ?? ?? ?? 4C 8D 47 18 48 8B CB");
  // Lua FastTravel.Stop reaches C_FastTravel at services + 0x128 -> + 0x28; its
  // Stop tests byte + 0x48, and the position getter reads + 0x74.
  natives.travel_stop =
      resolve_unique("FastTravelStop", "40 53 48 83 EC 20 8A 41 48 48 8B D9 90 84 C0 74 ?? E8");
  natives.travel_verified =
      resolve_unique("FastTravelStopBinding",
                     "40 53 48 83 EC 20 48 8B DA E8 ?? ?? ?? ?? B2 01 48 8B 88 28 01 00 00 48 "
                     "8B 49 28 E8") != nullptr &&
      natives.travel_stop != nullptr &&
      resolve_unique("FastTravelPosition",
                     "8B 41 7C F2 0F 10 41 74 F2 0F 11 02 89 42 08 48 8B C2 C3") != nullptr;
  log("TRAVEL state %s", natives.travel_verified ? "verified" : "unavailable");
  // Travel.canStart: the game's own check, and the "interior" tag it needs.
  kcds::runtime::world_map::travel_check::Natives check;
  check.services = get_services;
  check.check = resolve_unique("FastTravelCheck",
                               "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 50 48 8B 81 98 00 00 00 "
                               "48 8B F9 48 2B 81 90 00 00 00 48 B9 AB AA AA AA AA AA AA AA 48 C1 "
                               "F8 02");
  // The holder of the tags has no shape of its own: the second call of the game's route planning
  // just before it reads the "interior" tag's object.
  if (auto planning = static_cast<std::uint8_t*>(
          resolve_unique("FastTravelInteriorRead",
                         "E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B F8 E8 ?? ?? ?? ?? 48 8B 88 60 01 00 "
                         "00 48 8B 89 A8 00 00 00 48 8B 11 FF 52 10 48 8B 08 48 8B 11 48 8B C8 FF "
                         "D2"))) {
    std::int32_t displacement;
    std::memcpy(&displacement, planning + 6, 4);
    check.tag_manager = planning + 10 + displacement;
  }
  check.has_tag = resolve_unique("WorldHasTag",
                                 "48 89 5C 24 18 48 89 54 24 10 57 48 83 EC 30 48 8D 54 24 40 48 "
                                 "8B D9 E8 ?? ?? ?? ?? 48 8D 8B 80 01 00 00 48 8B F8 4C 8D 44 24 "
                                 "48 48 8D 54 24 20");
  check.make_string = resolve_unique("GameStringFromText",
                                     "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 48 83 EC "
                                     "20 44 8B 05 ?? ?? ?? ?? 48 8B F1");
  check.free_string = resolve_unique("GameStringFree", "48 83 EC 28 48 8B 09 48 83 C1 F4 E8");
  kcds::runtime::world_map::travel_check::install(check);
  log("TRAVEL check %s",
      kcds::runtime::world_map::travel_check::available() ? "installed" : "unavailable");
  natives.before_compass_add = before_compass_mark_add;
  natives.after_compass_add = after_compass_mark_add;
  const bool map = kcds::runtime::world_map::install(natives, log);
  log("WORLDMAP %s", map ? "installed" : "unavailable");
  // WorldMap.discover: the location manager's lookup by GUID, as the game's
  // DiscoverLocation calls it, and the manager's own vtable.
  kcds::runtime::world_map::discover::Natives discover;
  discover.services = get_services;
  discover.find_place = resolve_unique(
      "FindPlaceByGuid",
      "40 53 56 57 48 83 EC 30 49 8B F0 48 8B DA 48 8B F9 48 8D 15 ?? ?? ?? ?? 48 8B CE 41 B8 10 "
      "00 00 00 E8 ?? ?? ?? ?? 85 C0 74 46 48 8D 8F D8 01 00 00");
  const std::size_t manager =
      kcd2s::bridge::rtti_vtable(image, image_size, reinterpret_cast<std::uintptr_t>(image),
                                 ".?AVC_RPGLocationManager@rpgmodule@wh@@");
  discover.manager_vtable = manager == kcd2s::bridge::knpos ? nullptr : image + manager;
  kcds::runtime::world_map::discover::install(discover);
  log("WORLDMAP discover %s",
      discover.find_place && discover.manager_vtable ? "installed" : "unavailable");
}

}  // namespace kcds::game_client
