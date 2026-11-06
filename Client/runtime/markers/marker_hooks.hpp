// The game's own map, compass and discovery marks in a server world: the hooks that hide the
// single-player ones and draw the server's blips and the resources' points.
// Installed at startup by game_client.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcds::game_client {

// Discovery and codex toasts.
using DiscoveryHud = char(__fastcall*)(void*, const char* const*, const void*, std::uintptr_t,
                                       std::uintptr_t, std::uintptr_t);
extern DiscoveryHud original_discovery_hud;
char __fastcall hooked_discovery_hud(void* self, const char* const* name, const void* id,
                                     std::uintptr_t label, std::uintptr_t extra,
                                     std::uintptr_t guid);
using CodexToast = std::intptr_t(__fastcall*)(void*, const void*, int, const void*);
extern CodexToast original_codex_toast;
std::intptr_t __fastcall hooked_codex_toast(void* self, const void* label, int kind,
                                            const void* guid);

// The map's quest markers and the concept-only quest tipster marker.
using QuestMarkers = std::intptr_t(__fastcall*)(void*);
using QuestMarkerVisibility = std::intptr_t(__fastcall*)(void*, int, char);
extern QuestMarkers original_quest_markers;
extern QuestMarkerVisibility original_quest_marker_visibility;
std::intptr_t __fastcall hooked_quest_markers(void* self);
std::intptr_t __fastcall hooked_quest_marker_visibility(void* self, int quest, char visible);
using ConceptMapMarker = std::intptr_t(__fastcall*)(void*);
extern ConceptMapMarker original_concept_map_marker;
std::intptr_t __fastcall hooked_concept_map_marker(void* self);

// The map's clouds, acknowledged before they are read.
using MapCloudArray = std::intptr_t(__fastcall*)(void*, unsigned char, void*);
using AcknowledgeMapClouds = std::intptr_t(__fastcall*)(void*);
extern MapCloudArray original_map_cloud_array;
extern AcknowledgeMapClouds native_acknowledge_map_clouds;
std::intptr_t __fastcall hooked_map_cloud_array(void* self, unsigned char group, void* output);

// Which marker types the map and the compass show.
using MapMarkerVisible = bool(__fastcall*)(void*, int);
extern MapMarkerVisible original_map_marker_visible;
bool __fastcall hooked_map_marker_visible(void* self, int type);
using BlipViewRead = std::intptr_t(__fastcall*)(void*);
extern BlipViewRead original_compass_update;
extern BlipViewRead original_map_marker_build;
using CompassTypesOwner = void*(__fastcall*)(void* owner);
extern CompassTypesOwner compass_types_owner;
// Where the compass update calls the getter of its types table's owner.
inline constexpr std::size_t kCompassTypesCall = 0xEF;
// Where the map's marker build calls the registry walk, and where the walk calls the
// free of its point list.
inline constexpr std::size_t kMapRegistryWalkCall = 0x94;
inline constexpr std::size_t kRegistryWalkFreeCall = 0xA0;
std::intptr_t __fastcall hooked_compass_update(void* self);
std::intptr_t __fastcall hooked_map_marker_build(void* self);

// A point's map type and icon, a map point marker's icon name and a compass mark's type name.
using PointMapType = std::uint32_t(__fastcall*)(void* point);
extern PointMapType original_point_map_type;
extern PointMapType original_point_map_icon;
std::uint32_t __fastcall hooked_point_map_type(void* point);
std::uint32_t __fastcall hooked_point_map_icon(void* point);
using PoiMarkerConstruct = void*(__fastcall*)(void* marker, void* point);
extern PoiMarkerConstruct original_poi_marker_construct;
using CryStringAssign = void*(__fastcall*)(void* string, const char* text);
extern CryStringAssign poi_marker_name_assign;
void* __fastcall hooked_poi_marker_construct(void* marker, void* point);
using MarkTypeNameOf = const char*(__fastcall*)(int type);
extern MarkTypeNameOf original_mark_type_name;
const char* __fastcall hooked_mark_type_name(int type);
void before_compass_mark_add(void* mark);
void after_compass_mark_add();

}  // namespace kcds::game_client
