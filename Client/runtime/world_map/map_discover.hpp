// WorldMap.discover for client resources, WHGame 1.5.5: a place marked discovered on
// this player's map silently, without the game's toast, XP or discovery triggers.
//
// The game's discovery of a location (C_RPGLocationManager slot 32, which the flow
// node RPG:DiscoverLocation reaches by name through slot 48) looks the place up by its GUID
// (manager, &out, &guid; a reference-counted holder, the refcount at + 8), sets its
// status (the place at holder + 16: vtable + 80 the status, + 88 SetStatus; 0 undiscovered,
// 1 known but undiscovered, 2 discovered) and then tells the manager's listeners, which show the
// toast and give the XP. place() does the lookup and SetStatus(2) only. A point of interest
// (DiscoverPOI, slot 33,->) has a map of its own at the manager's + 32 and
// its status at + 140; place() writes the status as the point's setter does without
// telling the listeners.
//
// The game's DiscoverLocation, and the manager's status change of a point of interest (slot 49,
// which the point's setter calls), are hooked to know the key of the place being
// discovered when its toast fires (discovering_key).
//
// The manager is the RPG module's (services + 304) at + 184, checked by its vtable
// (.?AVC_RPGLocationManager@rpgmodule@wh@@) before use. A GUID travels as the game writes it,
// 8-4-4-4-12 hex digits (a 32-bit, two 16-bit, then eight single bytes).
//
// Engine thread.
#pragma once

#include <string>

namespace kcds::runtime::world_map::discover {

struct Natives {
  void* (*services)() = nullptr;
  void* find_place = nullptr;
  const void* manager_vtable = nullptr;  // C_RPGLocationManager's own vtable
};

void install(const Natives& natives);

// What discover() found.
enum class Outcome {
  Unknown,        // no place of that GUID in the loaded level, or no level
  Discovered,     // a location marked discovered now
  DiscoveredPoi,  // a point of interest marked discovered now
  Already,        // it was discovered before
};

// Marks the place of that GUID (8-4-4-4-12 hex digits) discovered, silently.
Outcome place(const std::string& guid);

// The GUID (8-4-4-4-12 hex digits) of the location the game discovers on this thread right now,
// empty outside its DiscoverLocation (hooked): the id the discovery toast's
// locationDiscovered hands out, which place() takes back. The toast shows another GUID of the
// location's, one the manager does not find it by.
std::string discovering_key(bool location);

}  // namespace kcds::runtime::world_map::discover
