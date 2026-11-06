#include "local_vegetation.hpp"

#include <chrono>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../core/natives.hpp"
#include "../level/level_edits.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::vegetation {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
using kcds::game_client::resolve_unique;

// The level loader laying an instance (+ 0x434): its physicalize (+ 7), the groups'
// global (+ 18), the group list's add (+ 45), the 3D engine's global (+ 52), the bending's
// setup (+ 76).
constexpr char kLoaderTail[] =
    "88 86 84 00 00 00 E8 ?? ?? ?? ?? 41 0F B7 0E 48 8B 15 ?? ?? ?? ?? 4C 69 C1 A0 01 00 00 48 "
    "8B 4A 10 48 8B D6 49 8B 8C 08 98 01 00 00 E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 11 4C "
    "8B 82 30 01 00 00 48 8B D6 41 FF D0 48 8B CE E8";
constexpr int kPhysicalizeCall = 7;
constexpr int kPhysicalizeEnd = 11;
constexpr int kGroupsLoad = 18;
constexpr int kGroupsLoadEnd = 22;
constexpr int kListAddCall = 45;
constexpr int kListAddEnd = 49;
constexpr int kEngineLoad = 52;
constexpr int kEngineLoadEnd = 56;
constexpr int kBendingCall = 76;
constexpr int kBendingEnd = 80;

constexpr std::size_t kGroupsBegin = 16;
constexpr std::size_t kGroupsEnd = 24;
constexpr std::size_t kGroupSize = 416;
constexpr std::size_t kGroupMesh = 0;
constexpr std::size_t kGroupList = 408;
// I3DEngine: CreateRenderNode(type) (slot 156) and RegisterEntity (slot 38); type 2 is
// CVegetation.
constexpr std::size_t kCreateRenderNode = 156;
constexpr std::size_t kRegisterEntity = 38;
constexpr int kVegetationType = 2;
// IStatObj: GetAABB(out min, max) (slot 4), GetFilePath (slot 41).
constexpr std::size_t kMeshBox = 4;
constexpr std::size_t kMeshFile = 41;
// IRenderNode: Hide (slot 68), Dephysicalize (slot 48).
constexpr std::size_t kNodeHide = 68;
constexpr std::size_t kNodeDephysicalize = 48;
// A CVegetation's fields.
constexpr std::size_t kPlace = 80;
constexpr std::size_t kGroup = 120;
constexpr std::size_t kTurn = 124;
constexpr std::size_t kScale = 125;
constexpr std::size_t kBoxUp = 126;
constexpr std::size_t kBoxDown = 129;
constexpr std::size_t kRadius = 132;
constexpr std::size_t kTiltX = 133;
constexpr std::size_t kTiltY = 134;
constexpr float kBoxBytesPerMetre = 3.984375f;
constexpr float kScaleUnit = 64.0f;
constexpr float kMaxScale = 255.0f / kScaleUnit;
constexpr float kMinScale = 1.0f / kScaleUnit;
constexpr std::size_t kMaxQueued = 256;
// The most plants planted at once (World Architect's Foliage and Add nature, a server's
// Vegetation.plant). 4096 stopped Add nature once a project had that many
// ("add works only somewhere, remove everywhere"); a level of the game holds far more.
constexpr std::size_t kMaxPlanted = 65536;

using Physicalize = void(__fastcall*)(void* node, bool instant);
using ListAdd = void(__fastcall*)(void* list, void* node);
using Bending = void(__fastcall*)(void* node);
using CreateRenderNode = void*(__fastcall*)(void* engine, int type);
using RegisterEntity = void(__fastcall*)(void* engine, void* node);
using MeshBox = void(__fastcall*)(void* mesh, float* box);
using MeshFile = const char*(__fastcall*)(void* mesh);
using NodeHide = void(__fastcall*)(void* node, bool hide);
using NodeDephysicalize = void(__fastcall*)(void* node, bool keep);

struct Request {
  std::int64_t id = 0;
  sdk::Value body;
};

struct Reply {
  std::int64_t id = 0;
  sdk::Value value;
};

Logger log_line = nullptr;
Physicalize physicalize = nullptr;
ListAdd list_add = nullptr;
Bending bending = nullptr;
std::uint8_t** groups_global = nullptr;
void** engine_global = nullptr;

std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<Reply> replies;
// What this game planted, by its id (engine thread).
std::map<std::uint32_t, void*> planted;
// Removed plants still to be taken out of the engine's physics, a few each frame (hundreds at
// once held the game's frame a second: owner, Clear nature over Add nature's grass).
std::vector<void*> leaving;
constexpr std::size_t kLeavingPerFrame = 64;
// Plants removed since the last report, and how many make one report line.
int removed_since_report = 0;
constexpr int kRemovedReport = 100;
std::uint32_t next_id = 1;

double number(const sdk::Value& value, const char* name, double fallback) {
  const sdk::Value* field = integration::script_field(value, name);
  if (!field) {
    return fallback;
  }
  if (field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  if (field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  return fallback;
}

// The level's vegetation groups, [begin, end) of 416-byte entries.
bool groups(std::uint8_t*& begin, std::uint8_t*& end) {
  std::uint8_t* manager = groups_global ? *groups_global : nullptr;
  if (!manager) {
    return false;
  }
  begin = *reinterpret_cast<std::uint8_t**>(manager + kGroupsBegin);
  end = *reinterpret_cast<std::uint8_t**>(manager + kGroupsEnd);
  return begin && end >= begin;
}

std::uint8_t byte_of(float metres) {
  const float value = metres * kBoxBytesPerMetre + 1.0f;
  return std::uint8_t((std::clamp)(value, 0.0f, 255.0f));
}

// A mesh path as the game keeps it, when it is plain printable text of a sane length.
std::string path_text(const char* path) {
  constexpr std::size_t kMaxPath = 260;
  std::string text;
  if (!path) {
    return text;
  }
  for (std::size_t i = 0; i < kMaxPath && path[i] != '\0'; ++i) {
    const char c = path[i];
    if (c < 32 || c > 126) {
      return std::string();
    }
    text.push_back(c);
  }
  return text;
}

sdk::Value answer_kinds() {
  std::uint8_t* begin = nullptr;
  std::uint8_t* end = nullptr;
  sdk::Value::Array kinds;
  if (!groups(begin, end)) {
    return sdk::Value();
  }
  for (std::uint8_t* group = begin; group < end; group += kGroupSize) {
    void* mesh = *reinterpret_cast<void**>(group + kGroupMesh);
    const char* path = mesh ? kcds::game_client::slot<MeshFile>(mesh, kMeshFile)(mesh) : nullptr;
    kinds.emplace_back(path_text(path));
  }
  if (log_line) {
    log_line("VEGETATION kinds %zu (first %s)", kinds.size(),
             kinds.empty() ? "none" : kinds.front().as_str().c_str());
  }
  return sdk::Value(sdk::Value::Map{{"kinds", sdk::Value(std::move(kinds))}});
}

// A mesh path lower-cased with forward slashes, and its file name without ".cgf".
std::string plain_path(std::string path) {
  for (char& c : path) {
    c = c == '\\' ? '/' : char(std::tolower(static_cast<unsigned char>(c)));
  }
  return path;
}

std::string stem_of(const std::string& path) {
  std::string stem = path.substr(path.find_last_of('/') + 1);
  constexpr char kExtension[] = ".cgf";
  if (stem.size() > 4 && stem.compare(stem.size() - 4, 4, kExtension) == 0) {
    stem.resize(stem.size() - 4);
  }
  return stem;
}

// The kind named by a mesh path or file name (as Vegetation.plant's kind): the group whose mesh
// has that path, else the first whose file name is the same; -1 when none has. One question per
// plant instead of the kinds list and the plant (Add nature lagged).
int kind_named(const std::string& name, std::uint8_t* begin, std::uint8_t* end, std::string& path) {
  const std::string wanted = plain_path(name);
  const std::string wanted_stem = stem_of(wanted);
  int by_stem = -1;
  std::string stem_path;
  int index = 0;
  for (std::uint8_t* group = begin; group < end; group += kGroupSize, ++index) {
    void* mesh = *reinterpret_cast<void**>(group + kGroupMesh);
    const char* file = mesh ? kcds::game_client::slot<MeshFile>(mesh, kMeshFile)(mesh) : nullptr;
    const std::string known = plain_path(path_text(file));
    if (known.empty()) {
      continue;
    }
    if (known == wanted) {
      path = known;
      return index;
    }
    if (by_stem < 0 && stem_of(known) == wanted_stem) {
      by_stem = index;
      stem_path = known;
    }
  }
  path = stem_path;
  return by_stem;
}

// `quiet`: one of many (localVegetationPlantMany logs them as one line: a line each held the
// game's frame up while the Architect's nature brush planted hundreds).
sdk::Value answer_plant(const sdk::Value& request, bool quiet = false) {
  std::uint8_t* begin = nullptr;
  std::uint8_t* end = nullptr;
  void* engine = engine_global ? *engine_global : nullptr;
  if (!groups(begin, end) || !engine || planted.size() >= kMaxPlanted) {
    return sdk::Value();
  }
  double kind = number(request, "kind", -1);
  std::string mesh_path;
  const sdk::Value* named = integration::script_field(request, "mesh");
  if (named && named->tag() == sdk::Tag::Str && !named->as_str().empty()) {
    kind = double(kind_named(named->as_str(), begin, end, mesh_path));
  }
  const std::size_t count = std::size_t(end - begin) / kGroupSize;
  if (kind < 0 || kind >= double(count)) {
    return sdk::Value();
  }
  std::uint8_t* group = begin + std::size_t(kind) * kGroupSize;
  void* mesh = *reinterpret_cast<void**>(group + kGroupMesh);
  if (!mesh) {
    return sdk::Value();
  }
  const float scale = (std::clamp)(float(number(request, "scale", 1)), kMinScale, kMaxScale);
  float box[6] = {};
  kcds::game_client::slot<MeshBox>(mesh, kMeshBox)(mesh, box);
  const float across =
      (std::max)({std::fabs(box[0]), std::fabs(box[1]), std::fabs(box[3]), std::fabs(box[4])}) *
      scale;
  const float up = (std::max)(box[5], 0.0f) * scale;
  const float down = (std::max)(-box[2], 0.0f) * scale;
  const auto started = std::chrono::steady_clock::now();
  auto* node = static_cast<std::uint8_t*>(kcds::game_client::slot<CreateRenderNode>(
      engine, kCreateRenderNode)(engine, kVegetationType));
  if (!node) {
    return sdk::Value();
  }
  auto* place = reinterpret_cast<float*>(node + kPlace);
  place[0] = float(number(request, "x", 0));
  place[1] = float(number(request, "y", 0));
  place[2] = float(number(request, "z", 0));
  *reinterpret_cast<int*>(node + kGroup) = int(kind);
  const double turn = std::fmod(number(request, "turn", 0), 360.0);
  node[kTurn] =
      std::uint8_t(int(std::lround((turn < 0 ? turn + 360.0 : turn) * 256.0 / 360.0)) & 255);
  node[kScale] = std::uint8_t(std::lround(scale * kScaleUnit));
  node[kBoxUp] = byte_of(across);
  node[kBoxUp + 1] = byte_of(across);
  node[kBoxUp + 2] = byte_of(up);
  node[kBoxDown] = byte_of(across);
  node[kBoxDown + 1] = byte_of(across);
  node[kBoxDown + 2] = byte_of(down);
  node[kRadius] =
      byte_of(std::sqrt(2.0f * across * across + (std::max)(up, down) * (std::max)(up, down)));
  node[kTiltX] = 0;
  node[kTiltY] = 0;
  physicalize(node, true);
  list_add(*reinterpret_cast<void**>(group + kGroupList), node);
  kcds::game_client::slot<RegisterEntity>(engine, kRegisterEntity)(engine, node);
  bending(node);
  const std::uint32_t id = next_id++;
  planted[id] = node;
  if (log_line && !quiet) {
    const auto took =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    log_line("VEGETATION planted %u kind %d at %.1f %.1f %.1f scale %.2f turn %.0f (%.2f ms)", id,
             int(kind), place[0], place[1], place[2], scale, turn, took);
  }
  if (mesh_path.empty()) {
    mesh_path = plain_path(path_text(kcds::game_client::slot<MeshFile>(mesh, kMeshFile)(mesh)));
  }
  return sdk::Value(sdk::Value::Map{
      {"id", sdk::Value(std::int64_t(id))},
      {"kind", sdk::Value(std::int64_t(kind))},
      {"mesh", sdk::Value(mesh_path)},
  });
}

bool remove_planted(std::uint32_t id) {
  const auto found = planted.find(id);
  if (found == planted.end()) {
    return false;
  }
  void* node = found->second;
  // Hidden at once; out of physics over the next frames (take_out_leaving).
  kcds::game_client::slot<NodeHide>(node, kNodeHide)(node, true);
  leaving.push_back(node);
  planted.erase(found);
  // Counted, not logged one by one (Clear nature takes hundreds a stroke).
  ++removed_since_report;
  if (log_line && removed_since_report >= kRemovedReport) {
    log_line("VEGETATION removed %d plants (the last %u)", removed_since_report, id);
    removed_since_report = 0;
  }
  return true;
}

// Takes removed plants out of the engine's on-demand physics, kLeavingPerFrame a frame: it would
// give a tree its collision back when a player comes near (a moved tree left an invisible wall
// where it stood).
void take_out_leaving() {
  const std::size_t count = (std::min)(leaving.size(), kLeavingPerFrame);
  for (std::size_t index = 0; index < count; ++index) {
    void* node = leaving[index];
    level::keep_vegetation_out(node, true);
    kcds::game_client::slot<NodeDephysicalize>(node, kNodeDephysicalize)(node, false);
  }
  leaving.erase(leaving.begin(), leaving.begin() + std::ptrdiff_t(count));
}

sdk::Value answer_one(const sdk::Value& request, bool in_world) {
  if (!in_world || !physicalize) {
    return sdk::Value();
  }
  const std::string op = integration::script_operation(request);
  if (op == "localVegetationKinds") {
    return answer_kinds();
  }
  if (op == "localVegetationPlant") {
    return answer_plant(request);
  }
  if (op == "localVegetationPlantMany") {
    // Many plants in one answer (World Architect's nature brush): each as localVegetationPlant.
    constexpr std::size_t kMostAtOnce = 256;
    const sdk::Value* plants = integration::script_field(request, "plants");
    const std::size_t before = planted.size();
    sdk::Value::Array answers;
    if (plants && plants->tag() == sdk::Tag::Array) {
      for (const sdk::Value& plant : plants->as_array()) {
        if (answers.size() >= kMostAtOnce) {
          break;
        }
        answers.push_back(plant.tag() == sdk::Tag::Map ? answer_plant(plant, true) : sdk::Value());
      }
    }
    if (log_line) {
      log_line("VEGETATION planted %zu of %zu asked", planted.size() - before, answers.size());
    }
    return sdk::Value(std::move(answers));
  }
  if (op == "localVegetationRemoveMany") {
    // Many removals in one answer (undo of a nature dab): true or false each.
    const sdk::Value* ids = integration::script_field(request, "ids");
    sdk::Value::Array answers;
    const auto started = std::chrono::steady_clock::now();
    if (ids && ids->tag() == sdk::Tag::Array) {
      for (const sdk::Value& id : ids->as_array()) {
        const bool known = id.tag() == sdk::Tag::Int;
        answers.push_back(sdk::Value(known && remove_planted(std::uint32_t(id.as_int()))));
      }
    }
    if (log_line) {
      const auto took =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
              .count();
      log_line("VEGETATION removed %zu at once (%.1f ms)", answers.size(), took);
    }
    return sdk::Value(std::move(answers));
  }
  if (op == "localVegetationRemove") {
    return sdk::Value(remove_planted(std::uint32_t(number(request, "id", 0))));
  }
  return sdk::Value();
}

}  // namespace

std::string kind_path(int kind) {
  std::uint8_t* begin = nullptr;
  std::uint8_t* end = nullptr;
  if (kind < 0 || !groups(begin, end) ||
      std::size_t(kind) >= std::size_t(end - begin) / kGroupSize) {
    return std::string();
  }
  void* mesh = *reinterpret_cast<void**>(begin + std::size_t(kind) * kGroupSize + kGroupMesh);
  return mesh ? plain_path(path_text(kcds::game_client::slot<MeshFile>(mesh, kMeshFile)(mesh)))
              : std::string();
}

bool install(Logger logger) {
  log_line = logger;
  physicalize = reinterpret_cast<Physicalize>(
      resolve_unique("VegetationPhysicalize", kLoaderTail, kPhysicalizeCall, kPhysicalizeEnd));
  groups_global = static_cast<std::uint8_t**>(
      resolve_unique("VegetationGroups", kLoaderTail, kGroupsLoad, kGroupsLoadEnd));
  list_add = reinterpret_cast<ListAdd>(
      resolve_unique("VegetationListAdd", kLoaderTail, kListAddCall, kListAddEnd));
  engine_global = static_cast<void**>(
      resolve_unique("VegetationEngine", kLoaderTail, kEngineLoad, kEngineLoadEnd));
  bending = reinterpret_cast<Bending>(
      resolve_unique("VegetationBending", kLoaderTail, kBendingCall, kBendingEnd));
  const bool found = physicalize && groups_global && list_add && engine_global && bending;
  if (!found) {
    physicalize = nullptr;
  }
  if (log_line) {
    log_line("VEGETATION planting %s", found ? "installed" : "unavailable");
  }
  return found;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      integration::script_operation(*request).rfind("localVegetation", 0) != 0) {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (requests.size() < kMaxQueued) {
    requests.push_back(Request{id->as_int(), *request});
  }
  return true;
}

void answer(bool in_world) {
  if (in_world) {
    take_out_leaving();
  }
  std::vector<Request> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(requests);
  }
  if (taken.empty()) {
    return;
  }
  std::vector<Reply> answered;
  for (const Request& request : taken) {
    answered.push_back(Reply{request.id, answer_one(request.body, in_world)});
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  for (Reply& reply : answered) {
    replies.push_back(std::move(reply));
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<Reply> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (const Reply& reply : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(reply.id)},
                                    {"value", reply.value}}));
  }
}

void reset() {
  planted.clear();
  // The world went with its plants.
  leaving.clear();
}

}  // namespace kcds::runtime::vegetation
