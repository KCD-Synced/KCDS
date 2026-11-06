#include "blips.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../world_map/resource_world_map.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::markers {
namespace {

// The 16 byte S_LocationId the registry keys its points by. The bed point is the bed entity id
// followed by "BED POI"; a blip is a tag of its own followed by the blip's id.
struct LocationId {
  std::uint64_t tag = 0;
  std::uint32_t blip = 0;
  std::uint32_t unused = 0;
};
static_assert(sizeof(LocationId) == 16);

using AddPoint = void*(__fastcall*)(void* manager, const LocationId* id, const float* position,
                                    float radius);
using RemovePoint = bool(__fastcall*)(void* manager, const LocationId* id);
using SetPointFlags = std::uint16_t(__fastcall*)(void* point, std::uint16_t flags, bool on);
using SetPointState = bool(__fastcall*)(void* point, std::uint32_t state, bool notify);
using TypeName = const char*(__fastcall*)(int type);
using StringAssign = void*(__fastcall*)(void* string, const char* text);

constexpr std::uint64_t kBlipTag = 0x50494C4253444B43ull;  // "KCDSBLIP"
// The client resources' own blips (the client Blip global) live under a tag of their own, and in
// `shown` under their id plus kOwnKey.
constexpr std::uint64_t kOwnBlipTag = 0x424E574F53444B43ull;  // "KCDSOWNB"
constexpr std::uint64_t kOwnKey = 1ull << 32;
constexpr std::size_t kMaxOwnRequests = 1024;
constexpr std::size_t kServicesRpgModule = 0x130;
constexpr std::size_t kRpgLocationManager = 0xB8;
constexpr std::size_t kPointType = 0x3C;
constexpr std::size_t kPointLabel = 0x50;
// The flags the game's bed point gets, and the one the compass and the map skip a point by.
constexpr std::uint16_t kPointFlags = 13;
constexpr std::uint16_t kPointHidden = 0x20;
constexpr std::uint32_t kKnownUndiscovered = 1;
constexpr std::uint32_t kDiscovered = 2;
// The point shape's radius. The compass finds a point within its range ratio times this radius.
constexpr float kPointRadius = 500.0f;
// The mark types the name table knows (0 to 96).
constexpr int kTypeCount = 97;

struct Kept {
  kcd2s::integration::BlipShow blip;
  std::uint64_t tag = kBlipTag;
  int type = -1;
  bool drawn = false;
  void* point = nullptr;  // its I_POI while drawn
};

// A client resource's blip shown or hidden, waiting for the engine thread.
struct OwnRequest {
  bool show = false;
  kcd2s::integration::BlipShow blip;
  std::string image;  // its own map icon's name, or empty
};

// The blips with an icon of their own, by their key in `shown`: the icon's name.
std::unordered_map<std::uint64_t, std::string> images;
// The server blips' icons the resource runtime is to send and mount: resource, path.
std::vector<std::pair<std::string, std::string>> wanted_icons;
// Whether an icon is mounted (native_ui map_icon_ready). A blip with an icon is drawn only once it
// is: the compass loads a mark's icon once, when the mark comes, and kept a missing-image box for
// a server blip whose image was still on its way.
bool (*icon_ready)(const std::string& name) = nullptr;

bool icon_waiting(std::uint64_t key) {
  const auto found = images.find(key);
  return found != images.end() && icon_ready && !icon_ready(found->second);
}

// The name the game client mounts a shipped .dds under, as the resource runtime and native_ui.cpp
// make it: kcds_<resource>_<path without .dds>, lower case, any other character '_'.
std::string icon_name(const std::string& resource, const std::string& path) {
  std::string stem = "kcds_" + resource + "_" + path;
  if (stem.size() >= 4 && _stricmp(stem.c_str() + stem.size() - 4, ".dds") == 0) {
    stem.resize(stem.size() - 4);
  }
  for (char& character : stem) {
    if (character >= 'A' && character <= 'Z') {
      character = char(character - 'A' + 'a');
    } else if (!(character >= 'a' && character <= 'z') && !(character >= '0' && character <= '9')) {
      character = '_';
    }
  }
  return stem;
}

BlipNatives natives;
BlipLogger report = nullptr;
bool ready = false;
std::unordered_map<std::string, int> type_by_name;
std::array<int, kTypeCount> drawn_per_type = {};
// A point was drawn or erased since the compass last read the registry.
bool points_changed = false;
// The mark type of each point drawn (by its I_POI), and the point of each id: the map reads a
// point's type from its location definition, which our points do not have.
std::unordered_map<const void*, int> point_types;
std::map<std::tuple<std::uint64_t, std::uint32_t, std::uint32_t>, const void*> points_by_id;
// The server's blips by their id, the client's own by their id plus kOwnKey.
std::unordered_map<std::uint64_t, Kept> shown;
// The client's own blips are drawn only while a world is ready (its registry is the game's).
bool own_blips_drawable = false;
// Own blips whose being drawn changed, for the resource runtime.
std::vector<std::pair<std::uint32_t, bool>> own_changes;
std::mutex own_mutex;
std::vector<OwnRequest> own_requests;

std::tuple<std::uint64_t, std::uint32_t, std::uint32_t> key_of(const LocationId& id) {
  return {id.tag, id.blip, id.unused};
}

LocationId id_of(const Kept& kept) {
  LocationId id;
  id.tag = kept.tag;
  id.blip = kept.blip.blip;
  return id;
}

void* location_manager() {
  auto* services = static_cast<std::uint8_t*>(natives.services());
  if (!services) {
    return nullptr;
  }
  auto* rpg = *reinterpret_cast<std::uint8_t**>(services + kServicesRpgModule);
  if (!rpg) {
    return nullptr;
  }
  return *reinterpret_cast<void**>(rpg + kRpgLocationManager);
}

std::uint32_t point_state_of(std::uint32_t mark_state) {
  if (mark_state == kKnownUndiscovered) {
    return kKnownUndiscovered;
  }
  return kDiscovered;
}

int type_of(const std::string& name) {
  const auto found = type_by_name.find(name);
  if (found == type_by_name.end()) {
    return -1;
  }
  return found->second;
}

// Makes the point under `id` in the registry, with the flags, the mark type, the label and the
// state. The point (its I_POI), or null when none was made.
void* draw_point(const LocationId& id, const float* position, int type, std::uint32_t state,
                 const char* label) {
  void* manager = location_manager();
  if (!manager) {
    return nullptr;
  }
  auto* point = static_cast<std::uint8_t*>(
      reinterpret_cast<AddPoint>(natives.add_point)(manager, &id, position, kPointRadius));
  if (!point) {
    return nullptr;
  }
  reinterpret_cast<SetPointFlags>(natives.set_point_flags)(point, kPointFlags, true);
  std::memcpy(point + kPointType, &type, sizeof(type));
  // The map marker's name; an empty one leaves the game's own fallback.
  reinterpret_cast<StringAssign>(natives.string_assign)(point + kPointLabel, label);
  reinterpret_cast<SetPointState>(natives.set_point_state)(point, state, false);
  ++drawn_per_type[std::size_t(type)];
  points_changed = true;
  point_types[point] = type;
  points_by_id[key_of(id)] = point;
  return point;
}

void erase_point(const LocationId& id, int type) {
  --drawn_per_type[std::size_t(type)];
  points_changed = true;
  if (const auto found = points_by_id.find(key_of(id)); found != points_by_id.end()) {
    point_types.erase(found->second);
    points_by_id.erase(found);
  }
  void* manager = location_manager();
  if (!manager) {
    return;
  }
  reinterpret_cast<RemovePoint>(natives.remove_point)(manager, &id);
}

bool draw(Kept& kept) {
  kept.point = draw_point(id_of(kept), kept.blip.position, kept.type,
                          point_state_of(kept.blip.markState), kept.blip.label.c_str());
  kept.drawn = kept.point != nullptr;
  return kept.drawn;
}

void erase(Kept& kept) {
  if (!kept.drawn) {
    return;
  }
  kept.drawn = false;
  kept.point = nullptr;
  erase_point(id_of(kept), kept.type);
}

// Shows `blip` under `key`, replacing what was shown there; `drawable` false only keeps it.
void place(std::uint64_t key, std::uint64_t tag, const kcd2s::integration::BlipShow& blip,
           bool drawable) {
  Kept& kept = shown[key];
  erase(kept);
  kept.blip = blip;
  kept.tag = tag;
  kept.type = type_of(blip.type);

  const char* outcome = "kept";
  if (!ready) {
    outcome = "kept (natives missing)";
  } else if (kept.type < 0) {
    outcome = "kept (unknown type)";
  } else if (!blip.compass && !blip.map) {
    outcome = "kept (neither compass nor map)";
  } else if (!drawable) {
    outcome = "kept (no world)";
  } else if (icon_waiting(key)) {
    outcome = "kept (its icon is on its way)";
  } else if (draw(kept)) {
    outcome = "drawn";
  } else {
    outcome = "kept (no location manager)";
  }
  if (report) {
    report(
        "BLIP %s%s %u type=%s(%d) state=%u compass=%d map=%d label=%zu at %.1f,%.1f,%.1f "
        "point=%p",
        tag == kOwnBlipTag ? "own " : "", outcome, blip.blip, blip.type.c_str(), kept.type,
        blip.markState, int(blip.compass), int(blip.map), blip.label.size(),
        double(blip.position[0]), double(blip.position[1]), double(blip.position[2]), kept.point);
  }
}

bool own_drawn(std::uint32_t blip) {
  const auto found = shown.find(kOwnKey | blip);
  return found != shown.end() && found->second.drawn;
}

void note_own(std::uint32_t blip, bool was_drawn) {
  const bool drawn = own_drawn(blip);
  if (drawn != was_drawn) {
    own_changes.emplace_back(blip, drawn);
  }
}

double number(const kcd2s::sdk::Value* value) {
  if (!value) {
    return 0;
  }
  if (value->tag() == kcd2s::sdk::Tag::Double) {
    return value->as_double();
  }
  if (value->tag() == kcd2s::sdk::Tag::Int) {
    return double(value->as_int());
  }
  return 0;
}

bool flag(const kcd2s::sdk::Value& message, const char* name) {
  const kcd2s::sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == kcd2s::sdk::Tag::Bool && value->as_bool();
}

std::string text(const kcd2s::sdk::Value& message, const char* name) {
  const kcd2s::sdk::Value* value = kcd2s::integration::script_field(message, name);
  return value && value->tag() == kcd2s::sdk::Tag::Str ? value->as_str() : std::string{};
}

// A clientBlipShow's position; false when it is not a finite { x, y, z }.
bool read_position(const kcd2s::sdk::Value& message, float position[3]) {
  const kcd2s::sdk::Value* value = kcd2s::integration::script_field(message, "position");
  if (!value || value->tag() != kcd2s::sdk::Tag::Map) {
    return false;
  }
  static const char* const kAxes[] = {"x", "y", "z"};
  for (int axis = 0; axis < 3; ++axis) {
    const kcd2s::sdk::Value* field = kcd2s::integration::script_field(*value, kAxes[axis]);
    if (!field) {
      return false;
    }
    position[axis] = float(number(field));
    if (!std::isfinite(position[axis])) {
      return false;
    }
  }
  return true;
}

// Quest objectives live under a tag of their own: the quest id, then the objective.
constexpr std::uint64_t kObjectiveTag = 0x4A424F5153444B43ull;  // "KCDSQOBJ"

LocationId objective_id_of(std::uint32_t quest, std::uint32_t objective) {
  LocationId id;
  id.tag = kObjectiveTag;
  id.blip = quest;
  id.unused = objective;
  return id;
}

std::uint64_t objective_key(std::uint32_t quest, std::uint32_t objective) {
  return std::uint64_t(quest) << 32 | objective;
}

// Each objective point drawn: its mark type and where it is.
struct ObjectivePoint {
  int type = -1;
  float position[3] = {0, 0, 0};
};
std::unordered_map<std::uint64_t, ObjectivePoint> objective_points;

// The mark types the map has no point art for (one blip of each type: these drew
// a missing-image box): the quest types 0 Checkpoint to 5 ActivityGiver and these by name. The
// map draws them as quest marks (map_quest_marks.hpp).
constexpr int kLastQuestMarkType = 5;
constexpr const char* kQuestMarkNames[] = {"Dlcs", "Bailiff", "FistFight", "Racing"};
std::array<bool, kTypeCount> quest_mark_types = {};

}  // namespace

bool install_blips(const BlipNatives& found, BlipLogger logger) {
  natives = found;
  report = logger;
  ready = natives.services && natives.add_point && natives.remove_point &&
          natives.set_point_flags && natives.set_point_state && natives.type_name &&
          natives.string_assign && natives.layout_verified;
  if (!ready) {
    return false;
  }
  const auto name_of = reinterpret_cast<TypeName>(natives.type_name);
  for (int type = 0; type < kTypeCount; ++type) {
    const char* name = name_of(type);
    if (name && *name) {
      type_by_name.emplace(name, type);
    }
  }
  for (int type = 0; type <= kLastQuestMarkType; ++type) {
    quest_mark_types[std::size_t(type)] = true;
  }
  for (const char* name : kQuestMarkNames) {
    const int type = type_of(name);
    if (type >= 0) {
      quest_mark_types[std::size_t(type)] = true;
    }
  }
  return true;
}

bool quest_mark_type(int type) {
  return type >= 0 && type < kTypeCount && quest_mark_types[std::size_t(type)];
}

void show_blip(const kcd2s::integration::BlipShow& blip) {
  if (blip.image_path.empty()) {
    images.erase(blip.blip);
  } else {
    images[blip.blip] = icon_name(blip.image_resource, blip.image_path);
    wanted_icons.emplace_back(blip.image_resource, blip.image_path);
  }
  place(blip.blip, kBlipTag, blip, true);
}

bool handle_runtime_message(const kcd2s::sdk::Value& message) {
  const std::string operation = kcd2s::integration::script_operation(message);
  if (operation != "clientBlipShow" && operation != "clientBlipHide") {
    return false;
  }
  OwnRequest request;
  request.show = operation == "clientBlipShow";
  const double id = number(kcd2s::integration::script_field(message, "blip"));
  if (!(id >= 1.0) || id > 4294967295.0) {
    return true;
  }
  request.blip.blip = std::uint32_t(id);
  if (request.show) {
    request.blip.type = text(message, "type");
    const double state = number(kcd2s::integration::script_field(message, "markState"));
    request.blip.markState = state >= 0.0 && state <= 4294967295.0 ? std::uint32_t(state) : 0;
    request.blip.label = text(message, "label");
    request.blip.compass = flag(message, "compass");
    request.blip.map = flag(message, "map");
    // Blip.legend: true unless the resource said false.
    const kcd2s::sdk::Value* legend = kcd2s::integration::script_field(message, "legend");
    request.blip.legend = !(legend && legend->tag() == kcd2s::sdk::Tag::Bool && !legend->as_bool());
    if (!read_position(message, request.blip.position)) {
      return true;
    }
    request.image = text(message, "image");
  }
  std::lock_guard<std::mutex> lock(own_mutex);
  if (own_requests.size() < kMaxOwnRequests) {
    own_requests.push_back(std::move(request));
  }
  return true;
}

void apply_own_blips() {
  std::vector<OwnRequest> requests;
  {
    std::lock_guard<std::mutex> lock(own_mutex);
    requests.swap(own_requests);
  }
  for (const OwnRequest& request : requests) {
    const std::uint32_t blip = request.blip.blip;
    const bool was_drawn = own_drawn(blip);
    if (request.show) {
      if (request.image.empty()) {
        images.erase(kOwnKey | blip);
      } else {
        images[kOwnKey | blip] = request.image;
      }
      place(kOwnKey | blip, kOwnBlipTag, request.blip, own_blips_drawable);
    } else if (const auto found = shown.find(kOwnKey | blip); found != shown.end()) {
      erase(found->second);
      shown.erase(found);
      images.erase(kOwnKey | blip);
    }
    note_own(blip, was_drawn);
  }
  draw_when_icons_came();
}

void set_icon_ready(bool (*ready)(const std::string& name)) {
  icon_ready = ready;
}

void draw_when_icons_came() {
  if (!ready || images.empty()) {
    return;
  }
  for (auto& [key, kept] : shown) {
    const bool own = kept.tag == kOwnBlipTag;
    if (kept.drawn || images.count(key) == 0 || icon_waiting(key) || kept.type < 0 ||
        (!kept.blip.compass && !kept.blip.map) || (own && !own_blips_drawable)) {
      continue;
    }
    if (draw(kept)) {
      if (report) {
        report("BLIP %s%u drawn, its icon came", own ? "own " : "", kept.blip.blip);
      }
      if (own) {
        own_changes.emplace_back(kept.blip.blip, true);
      }
    }
  }
}

bool image_at(const float position[3], std::string& name) {
  constexpr float kSamePlace = 0.05f;
  if (!position || images.empty()) {
    return false;
  }
  for (const auto& [key, image] : images) {
    const auto found = shown.find(key);
    if (found == shown.end() || !found->second.drawn) {
      continue;
    }
    const float* at = found->second.blip.position;
    if (std::fabs(at[0] - position[0]) < kSamePlace &&
        std::fabs(at[1] - position[1]) < kSamePlace) {
      name = image;
      return true;
    }
  }
  return false;
}

bool point_image(const void* point, std::string& name) {
  if (!point || images.empty()) {
    return false;
  }
  for (const auto& [key, kept] : shown) {
    if (kept.point != point) {
      continue;
    }
    const auto found = images.find(key);
    if (found == images.end()) {
      return false;
    }
    name = found->second;
    return true;
  }
  return false;
}

std::vector<std::pair<std::string, std::string>> take_wanted_icons() {
  return std::exchange(wanted_icons, {});
}

void own_blips_dropped() {
  own_blips_drawable = false;
  for (auto& [key, kept] : shown) {
    if (kept.tag != kOwnBlipTag || !kept.drawn) {
      continue;
    }
    // The registry went with the level: only our own bookkeeping of the point is undone.
    kept.drawn = false;
    kept.point = nullptr;
    --drawn_per_type[std::size_t(kept.type)];
    if (const auto found = points_by_id.find(key_of(id_of(kept))); found != points_by_id.end()) {
      point_types.erase(found->second);
      points_by_id.erase(found);
    }
    own_changes.emplace_back(kept.blip.blip, false);
  }
}

void redraw_own_blips() {
  own_blips_drawable = true;
  std::vector<kcd2s::integration::BlipShow> own;
  for (const auto& [key, kept] : shown) {
    if (kept.tag == kOwnBlipTag) {
      own.push_back(kept.blip);
    }
  }
  for (const auto& blip : own) {
    const bool was_drawn = own_drawn(blip.blip);
    place(kOwnKey | blip.blip, kOwnBlipTag, blip, true);
    note_own(blip.blip, was_drawn);
  }
}

void forget_own_blips() {
  own_blips_dropped();
  for (auto it = images.begin(); it != images.end();) {
    it = (it->first & kOwnKey) != 0 ? images.erase(it) : std::next(it);
  }
  for (auto it = shown.begin(); it != shown.end();) {
    it = it->second.tag == kOwnBlipTag ? shown.erase(it) : std::next(it);
  }
  own_changes.clear();
  std::lock_guard<std::mutex> lock(own_mutex);
  own_requests.clear();
}

std::vector<std::pair<std::uint32_t, bool>> take_own_blip_changes() {
  return std::exchange(own_changes, {});
}

bool hide_blip(std::uint32_t blip) {
  const auto found = shown.find(std::uint64_t(blip));
  if (found == shown.end()) {
    return false;
  }
  erase(found->second);
  shown.erase(found);
  images.erase(blip);
  if (report) {
    report("BLIP hidden %u", blip);
  }
  return true;
}

void forget_all_blips() {
  shown.clear();
  objective_points.clear();
  drawn_per_type.fill(0);
  point_types.clear();
  points_by_id.clear();
}

bool own_point_type(const void* point, int& type) {
  const auto found = point_types.find(point);
  if (found == point_types.end()) {
    return false;
  }
  type = found->second;
  return true;
}

bool show_objective_point(std::uint32_t quest, std::uint32_t objective, const float position[3],
                          int type) {
  hide_objective_point(quest, objective);
  if (!ready || type < 0 || type >= kTypeCount) {
    return false;
  }
  if (!draw_point(objective_id_of(quest, objective), position, type, kDiscovered, "")) {
    return false;
  }
  ObjectivePoint& point = objective_points[objective_key(quest, objective)];
  point.type = type;
  std::copy(position, position + 3, point.position);
  return true;
}

void hide_objective_point(std::uint32_t quest, std::uint32_t objective) {
  const auto found = objective_points.find(objective_key(quest, objective));
  if (found == objective_points.end()) {
    return;
  }
  erase_point(objective_id_of(quest, objective), found->second.type);
  objective_points.erase(found);
}

void forget_objective_points() {
  for (const auto& [key, point] : objective_points) {
    --drawn_per_type[std::size_t(point.type)];
  }
  objective_points.clear();
  for (auto it = points_by_id.begin(); it != points_by_id.end();) {
    if (std::get<0>(it->first) == kObjectiveTag) {
      point_types.erase(it->second);
      it = points_by_id.erase(it);
    } else {
      ++it;
    }
  }
}

void quest_marks(std::vector<QuestMark>& marks) {
  marks.clear();
  for (const auto& [key, kept] : shown) {
    if (!kept.drawn || !kept.blip.map || !quest_mark_type(kept.type)) {
      continue;
    }
    // A blip with its own icon is a point of the map like any other (own_point_image).
    if (images.count(key) != 0) {
      continue;
    }
    QuestMark mark;
    mark.key = key;
    mark.name = (kept.tag == kOwnBlipTag ? "kcds_own_blip_" : "kcds_blip_") +
                std::to_string(kept.blip.blip);
    mark.type = kept.type;
    std::copy(kept.blip.position, kept.blip.position + 3, mark.position);
    marks.push_back(std::move(mark));
  }
  for (const auto& [key, point] : objective_points) {
    if (!quest_mark_type(point.type)) {
      continue;
    }
    QuestMark mark;
    mark.key = (std::uint64_t(1) << 63) | key;
    mark.name = "kcds_quest_" + std::to_string(key >> 32);
    mark.objective = int(key & 0xffffffffu);
    mark.type = point.type;
    std::copy(point.position, point.position + 3, mark.position);
    marks.push_back(std::move(mark));
  }
}

std::size_t blip_count() {
  std::size_t count = 0;
  for (const auto& [key, kept] : shown) {
    if (kept.tag == kBlipTag) {
      ++count;
    }
  }
  return count;
}

bool blip_type_drawn(int type) {
  if (type < 0 || type >= kTypeCount) {
    return false;
  }
  return drawn_per_type[std::size_t(type)] > 0;
}

bool take_points_changed() {
  return std::exchange(points_changed, false);
}

void allow_drawn_types(std::uint8_t* table, bool allowed) {
  static std::array<std::uint8_t, kTypeCount> before{};
  static std::array<bool, kTypeCount> changed{};
  if (!table) {
    return;
  }
  for (std::size_t type = 0; type < std::size_t(kTypeCount); ++type) {
    if (allowed && drawn_per_type[type] > 0) {
      before[type] = table[type];
      changed[type] = true;
      table[type] = 1;
    } else if (!allowed && changed[type]) {
      table[type] = before[type];
      changed[type] = false;
    }
  }
}

namespace {

// The map's legend table has one byte for each type below this.
constexpr int kLegendTypes = 89;
// The location manager's list of every point (a vector of I_POI), and of an I_POI: whether it
// counts (slots 23 and 8, as the registry walk asks), its flag test (slot 14) and its
// map type (slot 29).
constexpr std::size_t kListPoints = 20;
constexpr std::size_t kPointCounts = 23;
constexpr std::size_t kPointKind = 8;
constexpr std::size_t kPointHasFlags = 14;
constexpr std::size_t kPointMapType = 29;

using ListPoints = void(__fastcall*)(void* manager, void** list);
using FreePoints = void(__fastcall*)(void* first, std::size_t bytes);
using PointCounts = bool(__fastcall*)(void* point);
using PointKind = std::uint32_t(__fastcall*)(void* point);
using PointHasFlags = bool(__fastcall*)(void* point, std::uint16_t flags);
using PointMapTypeOf = std::uint32_t(__fastcall*)(void* point);

// The types blips without `legend` let through while the map builds, and the points hidden for
// the legend then.
std::array<bool, kTypeCount> legend_forced = {};
std::vector<void*> legend_hidden;

template <class Fn>
Fn point_slot(void* object, std::size_t index) {
  return reinterpret_cast<Fn>((*static_cast<void***>(object))[index]);
}

bool switched_off(const std::uint8_t* table, int type) {
  if (kcds::runtime::world_map::category_choice(type) == 0) {
    return true;
  }
  return table && type < kLegendTypes && table[type] == 0;
}

// Hides the game's own points of these types (ours are in point_types).
void hide_game_points(const std::array<bool, kTypeCount>& types) {
  void* manager = location_manager();
  if (!manager || !natives.free_points) {
    return;
  }
  void* list[3] = {nullptr, nullptr, nullptr};
  point_slot<ListPoints>(manager, kListPoints)(manager, list);
  auto** first = static_cast<void**>(list[0]);
  auto** last = static_cast<void**>(list[1]);
  const auto set_flags = reinterpret_cast<SetPointFlags>(natives.set_point_flags);
  for (void** entry = first; entry != last; ++entry) {
    void* point = *entry;
    if (!point || point_types.count(point) != 0 ||
        !point_slot<PointCounts>(point, kPointCounts)(point) ||
        point_slot<PointKind>(point, kPointKind)(point) == 0) {
      continue;
    }
    const std::uint32_t type = point_slot<PointMapTypeOf>(point, kPointMapType)(point);
    if (type >= std::uint32_t(kTypeCount) || !types[type]) {
      continue;
    }
    if (point_slot<PointHasFlags>(point, kPointHasFlags)(point, kPointHidden)) {
      continue;
    }
    set_flags(point, kPointHidden, true);
    legend_hidden.push_back(point);
  }
  if (first) {
    const auto bytes =
        std::size_t(static_cast<std::uint8_t*>(list[2]) - static_cast<std::uint8_t*>(list[0])) &
        ~std::size_t(7);
    reinterpret_cast<FreePoints>(natives.free_points)(first, bytes);
  }
}

}  // namespace

void apply_legend(const std::uint8_t* table, bool building) {
  if (!ready) {
    return;
  }
  const auto set_flags = reinterpret_cast<SetPointFlags>(natives.set_point_flags);
  if (!building) {
    for (void* point : legend_hidden) {
      set_flags(point, kPointHidden, false);
    }
    legend_hidden.clear();
    legend_forced.fill(false);
    return;
  }
  for (auto& [id, kept] : shown) {
    const bool on_map = kept.drawn && kept.point && kept.blip.map;
    if (!on_map || kept.type < 0 || kept.type >= kTypeCount || !switched_off(table, kept.type)) {
      continue;
    }
    if (kept.blip.legend) {
      set_flags(kept.point, kPointHidden, true);
      legend_hidden.push_back(kept.point);
    } else {
      legend_forced[std::size_t(kept.type)] = true;
    }
  }
  // A type switched off that is let through for a blip (or an objective point) of it: the game's
  // own points of it stay hidden.
  std::array<bool, kTypeCount> through = {};
  bool any = false;
  for (int type = 0; type < kTypeCount; ++type) {
    if (switched_off(table, type) && drawn_per_type[std::size_t(type)] > 0) {
      through[std::size_t(type)] = true;
      any = true;
    }
  }
  if (any) {
    hide_game_points(through);
  }
}

bool legend_type_forced(int type) {
  return type >= 0 && type < kTypeCount && legend_forced[std::size_t(type)];
}

void hide_blips_off(BlipView view, bool hidden) {
  if (!ready) {
    return;
  }
  const auto set_flags = reinterpret_cast<SetPointFlags>(natives.set_point_flags);
  for (auto& [id, kept] : shown) {
    const bool for_view = view == BlipView::Compass ? kept.blip.compass : kept.blip.map;
    if (kept.drawn && kept.point && !for_view) {
      set_flags(kept.point, kPointHidden, hidden);
    }
  }
}

}  // namespace kcds::runtime::markers
