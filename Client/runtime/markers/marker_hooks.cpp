#include "marker_hooks.hpp"

#include "../core/client_internal.hpp"
#include "../core/log.hpp"
#include "../world_map/map_discover.hpp"
#include "../world_map/resource_world_map.hpp"
#include "blips.hpp"

#include <cstring>
#include <string>

namespace kcds::game_client {

// MCP: discovery toast methods enter this six-integer
// argument HUD bridge. Keep all other original notifications.
DiscoveryHud original_discovery_hud;
// The last argument is the place's GUID as text (a CryString, the location's and the
// point of interest's write it); its discovery goes to the scripts
// (locationDiscovered, poiDiscovered).
char __fastcall hooked_discovery_hud(void* self, const char* const* name, const void* id,
                                     std::uintptr_t label, std::uintptr_t extra,
                                     std::uintptr_t guid) {
  if (multiplayer_world.load() && name && *name && guid) {
    const bool location = std::strcmp(*name, "ShowDiscoveredLocation") == 0;
    if (location || std::strcmp(*name, "ShowDiscoveredPoi") == 0) {
      // A place goes by the key its manager finds it by (WorldMap.discover).
      const std::string key = kcds::runtime::world_map::discover::discovering_key(location);
      kcds::runtime::world_map::report_discovery(
          location, key.empty() ? *reinterpret_cast<const char* const*>(guid) : key.c_str());
    }
  }
  if (multiplayer_world.load() && name && *name &&
      (std::strcmp(*name, "ShowDiscoveredLocation") == 0 ||
       std::strcmp(*name, "ShowDiscoveredPoi") == 0 ||
       std::strcmp(*name, "ShowDiscoveredCodex") == 0))
    return 1;
  return original_discovery_hud(self, name, id, label, extra, guid);
}
// Codex discoveries use the separate four-argument HUD wrapper (C4B53C).
CodexToast original_codex_toast = nullptr;
std::intptr_t __fastcall hooked_codex_toast(void* self, const void* label, int kind,
                                            const void* guid) {
  if (multiplayer_world.load())
    return 0;
  return original_codex_toast(self, label, kind, guid);
}
// Map quest markers bypass the POI visibility predicate. Both original
// builders take borrowed arguments and create their temporary arrays internally.
QuestMarkers original_quest_markers = nullptr;
QuestMarkerVisibility original_quest_marker_visibility = nullptr;
std::intptr_t __fastcall hooked_quest_markers(void* self) {
  return multiplayer_world.load() ? 0 : original_quest_markers(self);
}
std::intptr_t __fastcall hooked_quest_marker_visibility(void* self, int quest, char visible) {
  return multiplayer_world.load() ? 0 : original_quest_marker_visibility(self, quest, visible);
}
// C_ShowMapMarker v43 (C4DADC) registers quest tipsters separately.
// Native QuestTipster maps to the same POI type6 as Hub; filtering type6
// cannot distinguish it from town labels. Suppress this concept-only path.
ConceptMapMarker original_concept_map_marker = nullptr;
std::intptr_t __fastcall hooked_concept_map_marker(void* self) {
  if (multiplayer_world.load()) {
    log("MAP concept marker suppressed");
    return 0;
  }
  return original_concept_map_marker(self);
}
// MCP normally serializes pending cloud states(1), THEN calls
// to acknowledge them as already revealed(2). A fully revealed
// multiplayer map must acknowledge first, so Flash receives settled state2.
MapCloudArray original_map_cloud_array = nullptr;
AcknowledgeMapClouds native_acknowledge_map_clouds = nullptr;
std::intptr_t __fastcall hooked_map_cloud_array(void* self, unsigned char group, void* output) {
  if (multiplayer_world.load())
    native_acknowledge_map_clouds(self);
  return original_map_cloud_array(self, group, output);
}
// native marker enum: 6 is Hub (town names). Multiplayer hides all other markers.
MapMarkerVisible original_map_marker_visible;
bool __fastcall hooked_map_marker_visible(void* self, int type) {
  // A blip without `legend` shows through its type's switch while the map builds.
  if (kcds::runtime::markers::legend_type_forced(type)) {
    return true;
  }
  if (multiplayer_world.load()) {
    // WorldMap.setMapCategory overrides the multiplayer default for its types.
    const int choice = kcds::runtime::world_map::category_choice(type);
    if (choice == 0)
      return false;
    // A blip the server shows needs its type through the filter.
    if (choice < 0 && type != 6 && !kcds::runtime::markers::blip_type_drawn(type))
      return false;
  }
  return original_map_marker_visible(self, type);
}
// The compass's update and the map's marker build read the game's points
// of interest; a blip the server shows on only one of them is hidden from the other while it
// reads (markers/blips.hpp).
BlipViewRead original_compass_update = nullptr;
BlipViewRead original_map_marker_build = nullptr;
// The object whose byte table (+ 6944, one per mark type) says which types the compass shows: the
// compass update gets it from its owner (+ 8) through, a call taken from the update's
// own code (the compiler made 23 copies of that getter).
CompassTypesOwner compass_types_owner = nullptr;
constexpr std::size_t kCompassTypesTable = 6944;
constexpr std::size_t kCompassRefresh = 248;
std::intptr_t __fastcall hooked_compass_update(void* self) {
  using kcds::runtime::markers::BlipView;
  std::uint8_t* types = nullptr;
  if (compass_types_owner && self) {
    void* owner = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + 8);
    auto* table_owner = static_cast<std::uint8_t*>(compass_types_owner(owner));
    types = table_owner ? table_owner + kCompassTypesTable : nullptr;
  }
  // The update's own "read again" mark (+ 248), set when our points changed.
  if (self && kcds::runtime::markers::take_points_changed()) {
    *(static_cast<std::uint8_t*>(self) + kCompassRefresh) = 1;
  }
  kcds::runtime::markers::hide_blips_off(BlipView::Compass, true);
  kcds::runtime::markers::allow_drawn_types(types, true);
  const std::intptr_t result = original_compass_update(self);
  kcds::runtime::markers::allow_drawn_types(types, false);
  kcds::runtime::markers::hide_blips_off(BlipView::Compass, false);
  return result;
}
// The map's own table of one byte per mark type (its legend's switches, at
// + 0x618 of the map; types below 89 index it as they are).
constexpr std::size_t kMapTypesTable = 0x618;
// The map's quest marks are updated (hooked_quest_markers keeps the game's own events
// from it in multiplayer) only on the quest system's tracking events, which our quests and blips
// never raise; after its points the map updates them too, so ours of the quest mark types are
// added (markers/map_quest_marks.hpp). The map's quest system is at + 880, as that update reads it.
constexpr std::size_t kMapQuestSystem = 880;
std::intptr_t __fastcall hooked_map_marker_build(void* self) {
  using kcds::runtime::markers::BlipView;
  std::uint8_t* types = self ? static_cast<std::uint8_t*>(self) + kMapTypesTable : nullptr;
  kcds::runtime::markers::hide_blips_off(BlipView::Map, true);
  // The legend's switches as the player left them, before the drawn types are turned on.
  kcds::runtime::markers::apply_legend(types, true);
  kcds::runtime::markers::allow_drawn_types(types, true);
  const std::intptr_t result = original_map_marker_build(self);
  kcds::runtime::markers::allow_drawn_types(types, false);
  kcds::runtime::markers::apply_legend(types, false);
  kcds::runtime::markers::hide_blips_off(BlipView::Map, false);
  void* quest_system =
      self ? *reinterpret_cast<void**>(static_cast<std::uint8_t*>(self) + kMapQuestSystem)
           : nullptr;
  if (original_quest_markers && quest_system) {
    original_quest_markers(self);
  }
  return result;
}
// The map reads a point's type (I_POI slot 29, -1 without) and its icon (slot 37,
// 0x7fffffff without) from the point's location definition. Our blips and objective
// points have none, so the map skipped them; for those both answer the type they were drawn
// with (a game shop answers 12 to both)..
// The quest mark types (0 Checkpoint to 5 ActivityGiver) have no point art on the map (a
// missing-image box beside the icon), so ours of those types stay off the map's points (-1); the
// map draws them as quest marks instead (markers/map_quest_marks.hpp).
PointMapType original_point_map_type = nullptr;
PointMapType original_point_map_icon = nullptr;
constexpr std::uint32_t kNoMapType = 0xffffffffu;
// A point of ours the map leaves to its quest marks: a quest mark type, without an icon of its own.
bool quest_marked_point(void* point, int type) {
  std::string image;
  return kcds::runtime::markers::quest_mark_type(type) &&
         !kcds::runtime::markers::point_image(point, image);
}
std::uint32_t __fastcall hooked_point_map_type(void* point) {
  int type = -1;
  if (kcds::runtime::markers::own_point_type(point, type)) {
    return quest_marked_point(point, type) ? kNoMapType : std::uint32_t(type);
  }
  return original_point_map_type(point);
}
std::uint32_t __fastcall hooked_point_map_icon(void* point) {
  int type = -1;
  if (kcds::runtime::markers::own_point_type(point, type)) {
    return quest_marked_point(point, type) ? 0x7fffffffu : std::uint32_t(type);
  }
  return original_point_map_icon(point);
}

// A blip's own map icon: the map's point marker (C_UIFlashMapPoiMarker, made from an
// I_POI) keeps the mark type's name at + 24, and the map loads the icon as
// img://Libs/UI/Textures/Icons/Map/<name>_icon.dds (ApseMap.gfx). For a point of a blip with an
// image the name is that image's, mounted there (native_ui.cpp mount_map_icon).
PoiMarkerConstruct original_poi_marker_construct = nullptr;
CryStringAssign poi_marker_name_assign = nullptr;
constexpr std::size_t kPoiMarkerName = 24;
void* __fastcall hooked_poi_marker_construct(void* marker, void* point) {
  void* result = original_poi_marker_construct(marker, point);
  std::string image;
  if (marker && poi_marker_name_assign && kcds::runtime::markers::point_image(point, image)) {
    poi_marker_name_assign(static_cast<std::uint8_t*>(marker) + kPoiMarkerName, image.c_str());
  }
  return result;
}

// The compass's mark (C_CompassMark) gives the HUD its type's name when it is added
// (AddCompassMarker, hooked by world_map/: the name of the type at + 76, from the type
// name table), and the HUD (hud.gfx) loads the icon from Libs/UI/Textures/Icons/Map/ by
// that name. The mark knows its place (+ 80) but not its point, so a mark at an own blip's place
// with an image is added with the image's name: the name table answers it while that mark is added.
MarkTypeNameOf original_mark_type_name = nullptr;
constexpr std::size_t kCompassMarkPosition = 80;
thread_local std::string mark_type_name_override;
const char* __fastcall hooked_mark_type_name(int type) {
  return mark_type_name_override.empty() ? original_mark_type_name(type)
                                         : mark_type_name_override.c_str();
}
void before_compass_mark_add(void* mark) {
  const auto* position =
      mark ? reinterpret_cast<const float*>(static_cast<std::uint8_t*>(mark) + kCompassMarkPosition)
           : nullptr;
  std::string image;
  if (original_mark_type_name && kcds::runtime::markers::image_at(position, image)) {
    mark_type_name_override = image;
  }
}
void after_compass_mark_add() {
  mark_type_name_override.clear();
}

}  // namespace kcds::game_client
