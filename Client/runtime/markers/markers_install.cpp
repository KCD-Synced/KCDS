// Winsock before Windows.h (see client_internal.hpp).
#include "../core/client_internal.hpp"

#include "markers_install.hpp"

#include <cstdint>
#include <cstring>
#include <string>

#include "../core/client_state.hpp"
#include "../core/log.hpp"
#include "../core/natives.hpp"
#include "../native_ui/native_ui.hpp"
#include "MinHook.h"
#include "blips.hpp"
#include "marker_hooks.hpp"
#include "world_markers.hpp"

namespace kcds::game_client {
namespace {

// The target of the call (E8) at this offset of a function, or null.
void* call_at(const void* function, std::size_t offset) {
  if (!function) {
    return nullptr;
  }
  const auto* call = static_cast<const std::uint8_t*>(function) + offset;
  if (call[0] != 0xE8) {
    return nullptr;
  }
  std::int32_t relative = 0;
  std::memcpy(&relative, call + 1, sizeof(relative));
  return const_cast<std::uint8_t*>(call + 5 + relative);
}

}  // namespace

DWORD resolve_map_ui_targets(StartupTargets& targets) {
  auto map_cloud_array =
      resolve_unique("NativeMapCloudArray",
                     "48 89 5C 24 08 48 89 74 24 18 57 48 83 EC 20 48 8B F9 0F B6 DA 48 8B 0D 90 "
                     "CA 4C 03 49 8B F0 48 8B 01 FF 90 18 01 00 00");
  native_acknowledge_map_clouds = reinterpret_cast<AcknowledgeMapClouds>(
      resolve_unique("NativeMapCloudAcknowledge",
                     "4C 8B 41 58 48 8B 51 50 EB 0B 48 8B 02 48 83 C2 08 C6 40 49 02 49 3B D0 75 "
                     "F0 48 8B 41 50 48 3B 41 58 74 04 48 89 41 58 C3"));
  if (!map_cloud_array || !native_acknowledge_map_clouds) {
    log("Native map cloud signatures missing");
    return 1;
  }
  auto codex_toast = resolve_unique("NativeCodexToast",
                                    "40 55 53 56 57 41 56 41 57 48 8B EC 48 83 EC 68 BF 04 00 00 "
                                    "00 C7 45 C8 00 00 00 00 40 84 3D B5 75 99 04 49 8B F1");
  auto quest_markers = resolve_unique(
      "NativeQuestMarkers",
      "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 "
      "4C 8B F9 48 8D 55 EF 48 8B 89 70 03 00 00 48 81 C1 C8 10 00 00");
  auto quest_marker_visibility =
      resolve_unique("NativeQuestMarkerVisibility",
                     "48 89 5C 24 18 48 89 74 24 20 89 54 24 10 55 57 41 54 41 56 41 57 48 8B EC "
                     "48 83 EC 70 41 8A D8 48 C7 45 F8 00 00 00 00 4C 8B F9 48 8D 91 80 06 00 00");
  if (!codex_toast || !quest_markers || !quest_marker_visibility) {
    log("Native codex/quest map signatures missing");
    return 1;
  }
  auto discovery_hud = resolve_unique(
      "NativeDiscoveryHud",
      "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 60 48 8B 01 49 8B F9 49 8B F0 "
      "4C 8B F2 48 8B D9 FF 50 60 48 8B 5B 48 48 85 DB 74 73 48 8D 4C 24 30 E8 D6 B0 BA FF");
  auto map_marker_visible = resolve_unique("NativeMapMarkerVisible",
                                           "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 8D 42 FC "
                                           "8B DA 83 F8 02 76 05 83 FA 45 75 20 E8 41 EE 77 01");
  if (!discovery_hud || !map_marker_visible) {
    log("Native map UI signatures missing");
    return 1;
  }
  targets.map_cloud_array = map_cloud_array;
  targets.codex_toast = codex_toast;
  targets.quest_markers = quest_markers;
  targets.quest_marker_visibility = quest_marker_visibility;
  targets.discovery_hud = discovery_hud;
  targets.map_marker_visible = map_marker_visible;
  return 0;
}

DWORD install_map_ui_hooks(const StartupTargets& targets) {
  // The targets under the names initialize_adapter() gave them.
  void* const map_cloud_array = targets.map_cloud_array;
  void* const codex_toast = targets.codex_toast;
  void* const quest_markers = targets.quest_markers;
  void* const quest_marker_visibility = targets.quest_marker_visibility;
  void* const discovery_hud = targets.discovery_hud;
  void* const map_marker_visible = targets.map_marker_visible;
  if (MH_CreateHook(map_cloud_array, reinterpret_cast<void*>(hooked_map_cloud_array),
                    reinterpret_cast<void**>(&original_map_cloud_array)) != MH_OK ||
      MH_EnableHook(map_cloud_array) != MH_OK)
    return 1;
  if (MH_CreateHook(codex_toast, reinterpret_cast<void*>(hooked_codex_toast),
                    reinterpret_cast<void**>(&original_codex_toast)) != MH_OK ||
      MH_EnableHook(codex_toast) != MH_OK)
    return 1;
  if (MH_CreateHook(quest_markers, reinterpret_cast<void*>(hooked_quest_markers),
                    reinterpret_cast<void**>(&original_quest_markers)) != MH_OK ||
      MH_EnableHook(quest_markers) != MH_OK)
    return 1;
  if (MH_CreateHook(quest_marker_visibility,
                    reinterpret_cast<void*>(hooked_quest_marker_visibility),
                    reinterpret_cast<void**>(&original_quest_marker_visibility)) != MH_OK ||
      MH_EnableHook(quest_marker_visibility) != MH_OK)
    return 1;
  if (MH_CreateHook(discovery_hud, reinterpret_cast<void*>(hooked_discovery_hud),
                    reinterpret_cast<void**>(&original_discovery_hud)) != MH_OK)
    return 1;
  if (MH_EnableHook(discovery_hud) != MH_OK)
    return 1;
  if (MH_CreateHook(map_marker_visible, reinterpret_cast<void*>(hooked_map_marker_visible),
                    reinterpret_cast<void**>(&original_map_marker_visible)) != MH_OK)
    return 1;
  if (MH_EnableHook(map_marker_visible) != MH_OK)
    return 1;
  return 0;
}

void install_markers() {
  // World markers: the game's decals, created through the 3D engine and removed
  // with the decal manager's DeleteDecalsInRange.
  kcds::runtime::markers::Natives marker_natives;
  marker_natives.engine_3d = engine_3d_slot;
  marker_natives.delete_decals_in_range = resolve_unique(
      "DeleteDecalsInRange",
      "48 89 5C 24 10 48 89 74 24 18 48 89 4C 24 08 55 57 41 54 41 56 41 57 48 8B EC 48 83 EC 70 "
      "48 8B 05 ?? ?? ?? ?? 4D 8B E0");
  log("MARKERS decals %s",
      kcds::runtime::markers::install(marker_natives, log) ? "installed" : "unavailable");
  // Blips: points of interest in the game's RPG location manager, made and removed
  // the way the game makes the player's bed point.
  kcds::runtime::markers::BlipNatives blip_natives;
  blip_natives.services = get_services;
  blip_natives.add_point = resolve_unique(
      "PointOfInterestAdd",
      "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 48 8B EC 48 83 EC 50 49 8B F0 0F 29 70 "
      "E8 4C 8B C2 48 8B DA 48 8B F9 48 8D 55 10 48 83 C1 20 0F 28 F3 E8 ?? ?? ?? ?? 48 8B 45 "
      "10 48 3B 47 28 0F 85");
  blip_natives.remove_point = resolve_unique(
      "PointOfInterestRemove",
      "48 89 5C 24 18 57 48 83 EC 30 48 8B D9 4C 8B C2 48 8D 54 24 20 48 83 C1 20 E8 ?? ?? ?? "
      "?? 48 8B 54 24 20 48 3B 53 28 75 04 32 C0 EB 48 48 8B 42 20");
  blip_natives.set_point_flags =
      resolve_unique("PointOfInterestFlags",
                     "0F B7 81 88 00 00 00 45 84 C0 74 0B 66 0B C2 66 89 81 88 00 00 00 C3");
  blip_natives.set_point_state = resolve_unique(
      "PointOfInterestState",
      "48 83 EC 28 44 8B 99 8C 00 00 00 4C 8B D1 44 3B DA 75 07 32 C0 48 83 C4 28 C3 89 91 8C "
      "00 00 00 45 84 C0 74 20 48 8B 05");
  blip_natives.type_name = resolve_unique(
      "MarkTypeName", "FF C1 83 F9 61 0F 87 ?? ?? ?? ?? 48 63 C1 48 8D 15 ?? ?? ?? ?? 8B 8C 82");
  // The compass reads the RPG module at services + 0x130 and its location manager
  // at + 0xB8; the C_POI constructor sets the mark type at C_POI + 0x4C (I_POI +
  // 0x3C) to -1.
  blip_natives.layout_verified =
      resolve_unique("CompassRpgModule",
                     "E8 ?? ?? ?? ?? 4C 8D B6 B8 00 00 00 4C 8D 7E 78 48 8B 98 30 01 00 00 4D "
                     "3B F7") != nullptr &&
      resolve_unique("CompassLocationManager",
                     "F3 0F 10 96 FC 00 00 00 4C 8D 4D ?? 48 C7 45 ?? 00 00 00 00 0F 57 C0 F3 "
                     "0F 7F 45 ?? 48 8B 8B B8 00 00 00 48 8D 90 20 1B 00 00") != nullptr &&
      resolve_unique("PointOfInterestType",
                     "48 89 43 38 48 89 73 40 89 73 48 C7 43 4C FF FF FF FF 0F 10 05") != nullptr;
  // The label is a CryString of the point, assigned as the game assigns one.
  blip_natives.string_assign = reinterpret_cast<void*>(string_assign);
  // The map's marker build, and the free of the registry's point list it gets through it
  // (Blip.legend), read before the build is hooked.
  auto map_marker_build = resolve_unique(
      "MapPoiMarkerBuild",
      "40 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 D0 48 81 EC 30 01 00 00 8B 15 ?? ?? ?? ?? "
      "0F 57 C0 65 48 8B 04 25 58 00 00 00 48 8B F1 B9 18 00 00");
  blip_natives.free_points =
      call_at(call_at(map_marker_build, kMapRegistryWalkCall), kRegistryWalkFreeCall);
  log("MARKERS registry list free %s", blip_natives.free_points ? "found" : "not found");
  log("MARKERS blips %s",
      kcds::runtime::markers::install_blips(blip_natives, log) ? "installed" : "unavailable");
  kcds::runtime::markers::set_icon_ready(
      [](const std::string& name) { return kcds::runtime::native_ui::map_icon_ready(name); });
  // The compass's update and the map's marker build, for blips on only one of them.
  auto compass_update = resolve_unique(
      "CompassUpdate",
      "48 8B C4 48 89 58 10 48 89 70 18 48 89 78 20 55 41 54 41 55 41 56 41 57 48 8D 68 A1 48 "
      "81 EC E0 00 00 00 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 45 2F 48 8D 55 8F 48 8B F1 E8 ?? "
      "?? ?? ?? 80 BE F8 00 00 00 00");
  // The compass's type table getter, from the update's own call to it (read before the hook
  // overwrites the update's first bytes; the call is far past them).
  if (compass_update) {
    const auto* call = static_cast<const std::uint8_t*>(compass_update) + kCompassTypesCall;
    if (call[0] == 0xE8) {
      std::int32_t relative = 0;
      std::memcpy(&relative, call + 1, sizeof(relative));
      compass_types_owner =
          reinterpret_cast<CompassTypesOwner>(const_cast<std::uint8_t*>(call + 5 + relative));
    }
    log("MARKERS compass type table %s", compass_types_owner ? "found" : "not found");
  }
  const bool views_apart =
      compass_update && map_marker_build &&
      MH_CreateHook(compass_update, reinterpret_cast<void*>(hooked_compass_update),
                    reinterpret_cast<void**>(&original_compass_update)) == MH_OK &&
      MH_CreateHook(map_marker_build, reinterpret_cast<void*>(hooked_map_marker_build),
                    reinterpret_cast<void**>(&original_map_marker_build)) == MH_OK &&
      MH_EnableHook(compass_update) == MH_OK && MH_EnableHook(map_marker_build) == MH_OK;
  // The map marker type of a point (I_POI slot 29, used only by C_POI).
  auto point_map_type = resolve_unique("PointMapType",
                                       "48 83 EC 28 48 83 C1 F0 E8 ?? ?? ?? ?? 48 85 C0 74 08 "
                                       "8B 40 18 48 83 C4 28 C3 83 C8 FF EB F6");
  const bool map_types =
      point_map_type &&
      MH_CreateHook(point_map_type, reinterpret_cast<void*>(hooked_point_map_type),
                    reinterpret_cast<void**>(&original_point_map_type)) == MH_OK &&
      MH_EnableHook(point_map_type) == MH_OK;
  auto point_map_icon = resolve_unique("PointMapIcon",
                                       "48 83 EC 28 48 83 C1 F0 E8 ?? ?? ?? ?? 48 85 C0 74 08 "
                                       "8B 40 30 48 83 C4 28 C3 B8 FF FF FF 7F EB F4");
  const bool map_icons =
      point_map_icon &&
      MH_CreateHook(point_map_icon, reinterpret_cast<void*>(hooked_point_map_icon),
                    reinterpret_cast<void**>(&original_point_map_icon)) == MH_OK &&
      MH_EnableHook(point_map_icon) == MH_OK;
  log("MARKERS blips on the map %s", map_types && map_icons ? "installed" : "unavailable");
  auto poi_marker_construct = resolve_unique(
      "MapPoiMarkerConstruct",
      "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 30 48 8B 02 48 8B F1 48 8B CA 48 8B FA FF 90 "
      "08 01 00 00 48 8B D0 48 8D 4C 24 48 E8");
  poi_marker_name_assign = reinterpret_cast<CryStringAssign>(string_assign);
  const bool blip_icons =
      poi_marker_construct &&
      MH_CreateHook(poi_marker_construct, reinterpret_cast<void*>(hooked_poi_marker_construct),
                    reinterpret_cast<void**>(&original_poi_marker_construct)) == MH_OK &&
      MH_EnableHook(poi_marker_construct) == MH_OK;
  log("MARKERS blip icons on the map %s", blip_icons ? "installed" : "unavailable");
  // The compass's own icons (the add is hooked by world_map/, which calls back around it).
  const bool compass_icons =
      blip_natives.type_name &&
      MH_CreateHook(blip_natives.type_name, reinterpret_cast<void*>(hooked_mark_type_name),
                    reinterpret_cast<void**>(&original_mark_type_name)) == MH_OK &&
      MH_EnableHook(blip_natives.type_name) == MH_OK;
  log("MARKERS blip icons on the compass %s", compass_icons ? "installed" : "unavailable");
  log("MARKERS blips compass and map apart %s",
      views_apart ? "installed" : "unavailable (a blip shows on both)");
}

}  // namespace kcds::game_client
