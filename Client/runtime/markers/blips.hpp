// Compass and map blips the server shows (kcd2s/integration/world_markers.hpp).
//
// A blip is drawn as a point of interest in the game's own registry, the RPG location manager
// (C_RPGLocationManager, services + 0x130 -> + 0xB8). The compass asks that registry
// every frame for the points near the player and makes a mark for each, and the map
// lists every point of it when it builds its markers, so a registered point shows on both. The
// game adds such a point of its own for the player's bed ("BED POI"); a blip does the
// same:
//
//   -(manager, id, position, radius) finds or makes the point (C_POI) under a 16 byte
//     location id and returns its I_POI (the C_POI + 0x10), placed as a point shape;
//   -(poi, 13, true) sets the flags the bed point gets (bit 0 makes it count);
//   - the mark type (the icon, the name table) is the int at I_POI + 0x3C, the field
//     the game's own point setup writes (C_POI constructor sets it to -1);
//   -(poi, state, false) sets the discovery state (E_LocationMarkStates: 0 Unknown,
//     1 KnownUndiscovered, 2 Discovered) without the discovery event; Unknown is never shown, so
//     a blip's markState 1 is KnownUndiscovered and every other value is Discovered;
//   - the label is the CryString at I_POI + 0x50: the map's marker takes its name from I_POI slot
//     33, that string, or "<Missing Label>" without one; it is assigned
//     with the game's CryString assign;
//   -(manager, id) takes the point out of the registry again.
//
// Compass and map apart: both skip a point whose flags (I_POI + 0x88) have bit 0x20. The compass
// asks its points in its update (through the registry's query and the filter
// !slot 22, flags & 0x20); the map lists them in its marker build (through
// slot 14 HasFlags(0x20) false). So while the compass updates, the blips without
// `compass` get bit 0x20, and while the map builds, the blips without `map`; it is taken off
// again right after, and the points are ordinary points the rest of the time.
//
// Limits: the multiplayer marker filter (game_client.cpp, hooked_map_marker_visible) lets a type
// through while a drawn blip has it, which also shows the game's own points of that type. A blip
// with neither `compass` nor `map` is kept but not drawn. The map shows a new blip the next time
// it is opened.
//
// Engine thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/world_markers.hpp"
#include "kcd2s/sdk/value.hpp"

namespace kcds::runtime::markers {

using BlipLogger = void (*)(const char* format, ...);

struct BlipNatives {
  void* (*services)() = nullptr;  // the game services getter
  void* add_point = nullptr;
  void* remove_point = nullptr;
  void* set_point_flags = nullptr;
  void* set_point_state = nullptr;
  void* type_name = nullptr;
  void* string_assign = nullptr;  // CryString = const char*
  void* free_points = nullptr;    // which frees the registry walk's point list
  bool layout_verified = false;   // the offsets above match this build
};

// False when a native is missing; blips are then kept but not drawn.
bool install_blips(const BlipNatives& natives, BlipLogger logger);

// Draws the blip, replacing the one shown earlier under the same id.
void show_blip(const kcd2s::integration::BlipShow& blip);

// Takes the blip away; true when the client had it.
bool hide_blip(std::uint32_t blip);

// Forgets every blip without touching the registry (the level it was in is gone).
void forget_all_blips();

// The client resources' own blips (the client Blip global, Client/resources/blips.cjs): the
// resource runtime asks with clientBlipShow { blip, type, markState, label, compass, map,
// position } and clientBlipHide { blip }. They are drawn like the server's under a location id
// tag of their own, and kept across levels: the game empties its registry with a level, so they
// count as not drawn when it goes and are drawn again when the next world is ready.
//
// A resource runtime message; false when it is not an own blip's. Any thread: it is queued.
bool handle_runtime_message(const kcd2s::sdk::Value& message);

// Shows and hides the own blips asked for since the last call.
void apply_own_blips();

// The level went: the own blips are kept but no longer drawn.
void own_blips_dropped();

// A world is ready: every own blip is drawn in its registry.
void redraw_own_blips();

// The session ended: the own blips are forgotten.
void forget_own_blips();

// The icon of its own of a blip's point (its I_POI): a blip with an image (the client's Blip
// `image`, Client/resources/blips.cjs, or the server's, BlipShow's image) is drawn on the map with
// Libs/UI/Textures/Icons/Map/<name>_icon.dds. False for every other point.
bool point_image(const void* point, std::string& name);

// The same for a compass mark, which knows its place only: the icon's name of the drawn blip with
// an image at `position` (x and y within 5 cm).
bool image_at(const float position[3], std::string& name);

// The server blips' images the resource runtime is to send and mount (resource, path), since the
// last call.
std::vector<std::pair<std::string, std::string>> take_wanted_icons();

// Tells whether an icon is mounted (native_ui map_icon_ready). A blip with an image is drawn only
// once its icon is there: the compass loads a mark's icon once, when the mark comes.
void set_icon_ready(bool (*ready)(const std::string& name));

// Draws the blips kept while their icon was on its way, now that it came; every frame (also done
// by apply_own_blips).
void draw_when_icons_came();

// The own blips whose being drawn (the compass carrying them) changed since the last call.
std::vector<std::pair<std::uint32_t, bool>> take_own_blip_changes();

// A followed quest's objective, drawn as the same kind of point under a location id
// of its own (the quest id, then the objective's index), Discovered, with the mark type the game
// gives the quest's journal section (Main 1, Side 2, Micro 3, Racing 86). Replaces
// the point drawn earlier for the objective; false when it could not be drawn.
bool show_objective_point(std::uint32_t quest, std::uint32_t objective, const float position[3],
                          int type);

// Takes the objective's point away, if it is drawn.
void hide_objective_point(std::uint32_t quest, std::uint32_t objective);

// Forgets every objective point without touching the registry (the level it was in is gone).
void forget_objective_points();

// A blip or objective point of a quest mark type (0 Checkpoint to 5 ActivityGiver) the map shows
// as a quest mark (map_quest_marks.hpp): the map has no point art for those types. `key` tells
// it from every other, `name` is its quest name there and `objective` its index in that quest
// (the map letters a quest's objectives A, B, ...).
struct QuestMark {
  std::uint64_t key = 0;
  std::string name;
  int objective = 0;
  int type = -1;
  float position[3] = {0, 0, 0};
};

// The drawn blips meant for the map and the objective points of quest mark types.
void quest_marks(std::vector<QuestMark>& marks);

// Whether the map draws a mark of this type as a quest mark: it has no point art for it (the quest
// types 0 to 5, Dlcs, Bailiff, FistFight and Racing).
bool quest_mark_type(int type);

// How many blips the server shows this client.
std::size_t blip_count();

// True while a drawn blip has the mark type (for the multiplayer marker filter).
bool blip_type_drawn(int type);

enum class BlipView { Compass, Map };

// Hides the drawn blips that are not for this view (true), around the view's own reading of the
// registry, and shows them again (false).
void hide_blips_off(BlipView view, bool hidden);

// Blip.legend: the map legend's switch for a mark type (the map's table of one byte
// per type at + 0x618, types below 89; or WorldMap.setMapCategory) hides the blips of that
// type on the map screen as it hides the game's own points, unless a blip's `legend` is false.
// Around the map's marker build (true): the drawn blips of a type switched off are hidden (bit
// 0x20) unless their `legend` is false, and the game's own points of a type switched off that a
// blip still shows are hidden instead, since the type is let through for it (allow_drawn_types
// turns its byte on, legend_type_forced answers the script's switch). The registry lists every
// point (location manager slot 20, a vector freed as its walk frees it), and the map's
// type of a point is I_POI slot 29. Afterwards (false) every point is given back.
void apply_legend(const std::uint8_t* table, bool building);

// True while the map builds and a blip without `legend` lets this type through a script's switch.
bool legend_type_forced(int type);

// The compass shows a point only when its mark type is on in a table of one byte per type (the
// filter reads it, at + 6944 of the object returns). Around the compass's
// update the types of the drawn blips and objective points are turned on (true) and given back
// what they had (false).
void allow_drawn_types(std::uint8_t* table, bool allowed);

// The mark type of a point we drew (its I_POI), false for the game's own points. The map takes a
// point's type from its location definition (I_POI slot 29), which ours lack, so it
// skipped them; the hook on slot 29 answers this type for ours.
bool own_point_type(const void* point, int& type);

// True once after a blip or objective point was drawn or erased: the compass reads the registry
// again only when the player moved a metre, so its update is then told to read it now.
bool take_points_changed();

}  // namespace kcds::runtime::markers
