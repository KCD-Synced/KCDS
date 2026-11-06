#include "resource_world_map.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <unordered_set>
#include <utility>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MinHook.h"
#include "map_discover.hpp"
#include "travel_check.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::world_map {
namespace {

namespace sdk = kcd2s::sdk;

constexpr std::size_t kModuleMap = 5384;
// Set while the map screen is up (the checkpoint hint refresh tests it); +1544
// stays set once the map has had a cursor.
constexpr std::size_t kMapOpen = 1864;
constexpr std::size_t kCheckpointsBegin = 1416;
constexpr std::size_t kCheckpointsEnd = 1424;
constexpr std::size_t kMarkPosition = 80;
constexpr std::size_t kMarkMapId = 120;
constexpr std::size_t kServicesMaps = 200;
constexpr std::size_t kServicesMapModule = 232;
constexpr std::size_t kServicesPlayerModule = 296;
constexpr std::size_t kPlayerFastTravel = 40;
constexpr std::size_t kTravelActive = 72;
constexpr std::size_t kTravelPosition = 116;

using MapCall = void(__fastcall*)(void* map);
using CompassAdd = double(__fastcall*)(void* mark, void* hud);
using CompassRemove = void*(__fastcall*)(void* mark, void* unused);
using Redraw = void(__fastcall*)(void* map, bool open);
using ModuleGetter = void*(__fastcall*)(void* registry);

Logger log_line = nullptr;
Natives natives;
MapCall original_set = nullptr;
MapCall original_remove = nullptr;
CompassAdd original_compass_add = nullptr;
CompassRemove original_compass_remove = nullptr;
// The compass marks on the strip; engine and UI code add and remove them.
std::mutex compass_mutex;
std::unordered_set<void*> compass_marks;

struct Waypoint {
  bool present = false;
  float x = 0;
  float y = 0;
  float z = 0;
  int map_id = -1;
};

struct Travel {
  bool active = false;
  float x = 0;
  float y = 0;
  float z = 0;
  int can_start = -1;  // the game's code, -1 when it cannot be asked
};

struct Event {
  std::string name;
  Waypoint where;
  bool moved = false;
};

// A discovered place: a location (or a point of interest) and its GUID.
struct Discovery {
  bool location = false;
  std::string guid;
};

std::mutex state_mutex;
std::vector<Discovery> discoveries;
bool open_now = false;
Waypoint waypoint_now;
std::vector<Event> events;
bool move_pending = false;
float move_x = 0;
float move_y = 0;
float move_z = 0;
bool clear_pending = false;
// WorldMap.clearAllFog asked; map_reveal.cpp takes it.
std::atomic<bool> fog_reveal_pending{false};
// WorldMap.discover's synchronous requests, answered on the engine thread: the
// request's id and the place's GUID, then the id and whether the GUID named a place.
struct DiscoverRequest {
  std::int64_t id = 0;
  std::string guid;
};
struct DiscoverReply {
  std::int64_t id = 0;
  bool found = false;
};
constexpr std::size_t kMaxDiscoverRequests = 64;
std::vector<DiscoverRequest> discover_requests;
std::vector<DiscoverReply> discover_replies;
// The points WorldMap.clearFog asked to clear (x, y), oldest first; map_reveal.cpp takes them.
constexpr std::size_t kMaxFogPoints = 256;
std::vector<std::array<float, 2>> fog_points;

const void* reported_session = nullptr;
bool reported = false;
bool reported_open = false;
std::map<int, bool> categories;  // mark type -> shown, set by resources
std::size_t compass_now = 0;
std::size_t reported_compass = 0;
Travel travel_now;
bool in_world_now = false;
bool reported_in_world = false;
Travel reported_travel;
Waypoint reported_waypoint;

void* map_object() {
  void* services = natives.services ? natives.services() : nullptr;
  if (!services) {
    return nullptr;
  }
  void* registry =
      *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + kServicesMapModule);
  if (!registry) {
    return nullptr;
  }
  auto module =
      static_cast<std::uint8_t*>(reinterpret_cast<ModuleGetter>(natives.module_getter)(registry));
  return module ? module + kModuleMap : nullptr;
}

int current_map_id() {
  void* services = natives.services ? natives.services() : nullptr;
  if (!services) {
    return -1;
  }
  void* maps = *reinterpret_cast<void**>(static_cast<std::uint8_t*>(services) + kServicesMaps);
  if (!maps) {
    return -1;
  }
  using Getter = void*(__fastcall*)(void*);
  auto current = (*reinterpret_cast<Getter**>(maps))[0](maps);
  if (!current) {
    return -1;
  }
  using IdGetter = int(__fastcall*)(void*);
  return (*reinterpret_cast<IdGetter**>(current))[0](current);
}

// The current map's mark, read as does but without taking a reference.
std::uint8_t* current_mark(void* map) {
  auto bytes = static_cast<std::uint8_t*>(map);
  auto begin = *reinterpret_cast<std::uint8_t**>(bytes + kCheckpointsBegin);
  auto end = *reinterpret_cast<std::uint8_t**>(bytes + kCheckpointsEnd);
  const int map_id = current_map_id();
  for (auto entry = begin; entry && entry < end; entry += 16) {
    auto mark = *reinterpret_cast<std::uint8_t**>(entry);
    if (mark && *reinterpret_cast<int*>(mark + kMarkMapId) == map_id) {
      return mark;
    }
  }
  return nullptr;
}

Waypoint read_waypoint(void* map) {
  Waypoint out;
  std::uint8_t* mark = map ? current_mark(map) : nullptr;
  if (!mark) {
    return out;
  }
  auto position = reinterpret_cast<float*>(mark + kMarkPosition);
  out.present = true;
  out.x = position[0];
  out.y = position[1];
  out.z = position[2];
  out.map_id = *reinterpret_cast<int*>(mark + kMarkMapId);
  return out;
}

void push_event(const char* name, const Waypoint& where, bool moved) {
  std::lock_guard<std::mutex> lock(state_mutex);
  if (events.size() < 64) {
    events.push_back(Event{name, where, moved});
  }
}

void __fastcall hooked_set(void* map) {
  const Waypoint before = read_waypoint(map);
  original_set(map);
  const Waypoint after = read_waypoint(map);
  if (after.present) {
    push_event("mapWaypointSet", after, before.present);
  }
}

void __fastcall hooked_remove(void* map) {
  const Waypoint before = read_waypoint(map);
  original_remove(map);
  if (before.present && !read_waypoint(map).present) {
    push_event("mapWaypointCleared", before, false);
  }
}

double __fastcall hooked_compass_add(void* mark, void* hud) {
  {
    std::lock_guard<std::mutex> lock(compass_mutex);
    compass_marks.insert(mark);
  }
  if (natives.before_compass_add) {
    natives.before_compass_add(mark);
  }
  const double result = original_compass_add(mark, hud);
  if (natives.after_compass_add) {
    natives.after_compass_add();
  }
  return result;
}

void* __fastcall hooked_compass_remove(void* mark, void* unused) {
  {
    std::lock_guard<std::mutex> lock(compass_mutex);
    compass_marks.erase(mark);
  }
  return original_compass_remove(mark, unused);
}

Travel read_travel() {
  Travel out;
  void* services = natives.travel_verified && natives.services ? natives.services() : nullptr;
  if (!services) {
    return out;
  }
  auto player = *reinterpret_cast<std::uint8_t**>(static_cast<std::uint8_t*>(services) +
                                                  kServicesPlayerModule);
  auto travel = player ? *reinterpret_cast<std::uint8_t**>(player + kPlayerFastTravel) : nullptr;
  if (!travel || !travel[kTravelActive]) {
    return out;
  }
  auto position = reinterpret_cast<float*>(travel + kTravelPosition);
  out.active = true;
  out.x = position[0];
  out.y = position[1];
  out.z = position[2];
  return out;
}

// The game's own answer to "could a travel begin", asked four times a second.
int can_start_now() {
  constexpr auto kAskEvery = std::chrono::milliseconds(250);
  static std::chrono::steady_clock::time_point asked_at;
  static int answer = -1;
  const auto now = std::chrono::steady_clock::now();
  if (now - asked_at >= kAskEvery) {
    asked_at = now;
    answer = travel_check::ask();
  }
  return answer;
}

bool same_travel(const Travel& a, const Travel& b) {
  return a.active == b.active && a.x == b.x && a.y == b.y && a.z == b.z &&
         a.can_start == b.can_start;
}

bool same(const Waypoint& a, const Waypoint& b) {
  return a.present == b.present &&
         (!a.present || (a.x == b.x && a.y == b.y && a.z == b.z && a.map_id == b.map_id));
}

sdk::Value position_map(const Waypoint& where) {
  sdk::Value::Map out{{"x", sdk::Value(static_cast<double>(where.x))},
                      {"y", sdk::Value(static_cast<double>(where.y))},
                      {"z", sdk::Value(static_cast<double>(where.z))},
                      {"mapId", sdk::Value(static_cast<std::int64_t>(where.map_id))}};
  return sdk::Value(std::move(out));
}

bool hook(void* target, void* detour, void** original) {
  return target && MH_CreateHook(target, detour, original) == MH_OK &&
         MH_EnableHook(target) == MH_OK;
}

}  // namespace

bool install(const Natives& resolved, Logger logger) {
  natives = resolved;
  log_line = logger;
  if (!natives.services || !natives.module_getter || !natives.refresh_hints || !natives.redraw) {
    return false;
  }
  return hook(natives.set_checkpoint, reinterpret_cast<void*>(hooked_set),
              reinterpret_cast<void**>(&original_set)) &&
         hook(natives.remove_checkpoint, reinterpret_cast<void*>(hooked_remove),
              reinterpret_cast<void**>(&original_remove)) &&
         hook(natives.compass_add, reinterpret_cast<void*>(hooked_compass_add),
              reinterpret_cast<void**>(&original_compass_add)) &&
         hook(natives.compass_remove, reinterpret_cast<void*>(hooked_compass_remove),
              reinterpret_cast<void**>(&original_compass_remove));
}

int category_choice(int type) {
  std::lock_guard<std::mutex> lock(state_mutex);
  auto found = categories.find(type);
  return found == categories.end() ? -1 : (found->second ? 1 : 0);
}

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation == "syncRequest") {
    const sdk::Value* id = kcd2s::integration::script_field(message, "id");
    const sdk::Value* request = kcd2s::integration::script_field(message, "request");
    if (!id || id->tag() != sdk::Tag::Int || !request ||
        kcd2s::integration::script_operation(*request) != "worldMapDiscover") {
      return false;  // another module's synchronous request
    }
    const sdk::Value* guid = kcd2s::integration::script_field(*request, "guid");
    std::lock_guard<std::mutex> lock(state_mutex);
    if (discover_requests.size() < kMaxDiscoverRequests) {
      discover_requests.push_back(DiscoverRequest{
          id->as_int(), guid && guid->tag() == sdk::Tag::Str ? guid->as_str() : std::string()});
    }
    return true;
  }
  if (operation == "worldMapCategories") {
    std::map<int, bool> next;
    for (const char* field : {"shown", "hidden"}) {
      const sdk::Value* list = kcd2s::integration::script_field(message, field);
      if (!list || list->tag() != sdk::Tag::Array) {
        continue;
      }
      for (const sdk::Value& value : list->as_array()) {
        if (value.tag() == sdk::Tag::Int) {
          next[static_cast<int>(value.as_int())] = std::string(field) == "shown";
        }
      }
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    categories = std::move(next);
    return true;
  }
  if (operation == "worldMapClear") {
    std::lock_guard<std::mutex> lock(state_mutex);
    clear_pending = true;
    return true;
  }
  if (operation == "worldMapRevealFog") {
    fog_reveal_pending.store(true);
    return true;
  }
  if (operation != "worldMapMove" && operation != "worldMapRevealFogAt") {
    return false;
  }
  auto number = [&](const char* name, float& out) {
    const sdk::Value* value = kcd2s::integration::script_field(message, name);
    if (value && value->tag() == sdk::Tag::Double) {
      out = static_cast<float>(value->as_double());
      return true;
    }
    if (value && value->tag() == sdk::Tag::Int) {
      out = static_cast<float>(value->as_int());
      return true;
    }
    return false;
  };
  float x = 0;
  float y = 0;
  float z = 0;
  if (operation == "worldMapRevealFogAt") {
    if (number("x", x) && number("y", y)) {
      std::lock_guard<std::mutex> lock(state_mutex);
      if (fog_points.size() < kMaxFogPoints) {
        fog_points.push_back({x, y});
      }
    }
    return true;
  }
  if (number("x", x) && number("y", y) && number("z", z)) {
    std::lock_guard<std::mutex> lock(state_mutex);
    move_pending = true;
    move_x = x;
    move_y = y;
    move_z = z;
  }
  return true;
}

bool take_fog_reveal() {
  return fog_reveal_pending.exchange(false);
}

std::vector<std::array<float, 2>> take_fog_points() {
  std::lock_guard<std::mutex> lock(state_mutex);
  return std::exchange(fog_points, {});
}

void frame(bool in_world) {
  // WorldMap.discover: each place marked discovered silently, the answer to its script.
  std::vector<DiscoverRequest> asked;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    asked.swap(discover_requests);
  }
  for (const DiscoverRequest& request : asked) {
    const discover::Outcome outcome =
        in_world ? discover::place(request.guid) : discover::Outcome::Unknown;
    const bool marked =
        outcome == discover::Outcome::Discovered || outcome == discover::Outcome::DiscoveredPoi;
    if (marked && log_line) {
      log_line("WORLDMAP %s discovered by a script", request.guid.c_str());
    }
    // Marking a point of interest fires poiDiscovered; locations fire no event.
    if (outcome == discover::Outcome::DiscoveredPoi) {
      report_discovery(false, request.guid.c_str());
    }
    std::lock_guard<std::mutex> lock(state_mutex);
    discover_replies.push_back(DiscoverReply{request.id, outcome != discover::Outcome::Unknown});
  }
  void* map = in_world && original_set ? map_object() : nullptr;
  bool move = false;
  bool clear = false;
  float x = 0;
  float y = 0;
  float z = 0;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    move = move_pending;
    clear = clear_pending;
    move_pending = false;
    clear_pending = false;
    x = move_x;
    y = move_y;
    z = move_z;
  }
  const bool open = map && *(static_cast<std::uint8_t*>(map) + kMapOpen) != 0;
  if (map && clear && current_mark(map)) {
    // The game's own removal, so the HUD, the hints and the event follow as for the player.
    hooked_remove(map);
  }
  if (map && move) {
    if (std::uint8_t* mark = current_mark(map)) {
      auto position = reinterpret_cast<float*>(mark + kMarkPosition);
      position[0] = x;
      position[1] = y;
      position[2] = z;
      reinterpret_cast<MapCall>(natives.refresh_hints)(map);
      reinterpret_cast<Redraw>(natives.redraw)(map, open);
    }
  }
  const Waypoint waypoint = map ? read_waypoint(map) : Waypoint{};
  std::size_t marks = 0;
  {
    std::lock_guard<std::mutex> lock(compass_mutex);
    // A level change rebuilds the HUD without taking the old marks off one by one.
    if (!in_world) {
      compass_marks.clear();
    }
    marks = compass_marks.size();
  }
  Travel travel = in_world ? read_travel() : Travel{};
  travel.can_start = in_world ? can_start_now() : -1;
  std::lock_guard<std::mutex> lock(state_mutex);
  open_now = open;
  waypoint_now = waypoint;
  compass_now = marks;
  travel_now = travel;
  in_world_now = in_world;
}

// A GUID as the game writes one: 8-4-4-4-12 hex digits.
bool guid_text(const char* text) {
  constexpr std::size_t kLength = 36;
  if (!text) {
    return false;
  }
  for (std::size_t i = 0; i < kLength; ++i) {
    const char c = text[i];
    const bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    if (dash ? c != '-' : !hex) {
      return false;
    }
  }
  return text[kLength] == '\0';
}

void report_discovery(bool location, const char* guid) {
  if (!guid_text(guid)) {
    return;
  }
  std::lock_guard<std::mutex> lock(state_mutex);
  if (discoveries.size() < 64) {
    discoveries.push_back(Discovery{location, guid});
  }
}

void pump(const void* session, const RuntimeSender& send) {
  bool open = false;
  Waypoint waypoint;
  std::size_t compass = 0;
  Travel travel;
  bool in_world = false;
  std::vector<Event> taken;
  std::vector<Discovery> found;
  std::vector<DiscoverReply> answers;
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    found.swap(discoveries);
    answers.swap(discover_replies);
    open = open_now;
    waypoint = waypoint_now;
    compass = compass_now;
    travel = travel_now;
    in_world = in_world_now;
    taken.swap(events);
  }
  if (session != reported_session) {
    reported_session = session;
    reported = false;
    std::lock_guard<std::mutex> lock(state_mutex);
    categories.clear();
  }
  if (!reported || open != reported_open || !same(waypoint, reported_waypoint) ||
      compass != reported_compass || !same_travel(travel, reported_travel) ||
      in_world != reported_in_world) {
    sdk::Value::Map travel_state{
        {"active", sdk::Value(travel.active)},
        {"x", sdk::Value(static_cast<double>(travel.x))},
        {"y", sdk::Value(static_cast<double>(travel.y))},
        {"z", sdk::Value(static_cast<double>(travel.z))},
        {"canStart", travel.can_start < 0
                         ? sdk::Value()
                         : sdk::Value(static_cast<std::int64_t>(travel.can_start))}};
    sdk::Value::Map state{{"op", sdk::Value(std::string("worldMap"))},
                          {"open", sdk::Value(open)},
                          {"waypoint", waypoint.present ? position_map(waypoint) : sdk::Value()},
                          {"compassMarks", sdk::Value(static_cast<std::int64_t>(compass))},
                          {"inWorld", sdk::Value(in_world)},
                          {"travel", sdk::Value(std::move(travel_state))}};
    if (send(sdk::Value(std::move(state)))) {
      reported = true;
      reported_open = open;
      reported_waypoint = waypoint;
      reported_compass = compass;
      reported_travel = travel;
      reported_in_world = in_world;
    }
  }
  for (const Event& event : taken) {
    sdk::Value::Map message{{"op", sdk::Value(std::string("worldMapEvent"))},
                            {"name", sdk::Value(event.name)},
                            {"where", position_map(event.where)},
                            {"moved", sdk::Value(event.moved)}};
    send(sdk::Value(std::move(message)));
  }
  for (const DiscoverReply& answer : answers) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(answer.id)},
                                    {"value", sdk::Value(answer.found)}}));
  }
  for (Discovery& place : found) {
    sdk::Value::Map message{{"op", sdk::Value(std::string("worldMapDiscovered"))},
                            {"kind", sdk::Value(std::string(place.location ? "location" : "poi"))},
                            {"id", sdk::Value(std::move(place.guid))}};
    send(sdk::Value(std::move(message)));
  }
}

}  // namespace kcds::runtime::world_map
