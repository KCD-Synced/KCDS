#include "navmesh_paths.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>

#include "../core/natives.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "navmesh_holes.hpp"
// Last: it brings Windows.h, whose min and max macros the headers above must not see.
#include "MinHook.h"

namespace kcds::runtime::world::navmesh_paths {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
using kcds::game_client::resolve_unique;

// A script's question waiting for the engine thread, and the most that wait.
struct Question {
  std::int64_t id = 0;
  bool nearest = false;
  float from[3] = {};
  float to[3] = {};
  float reach = kNearReach;
};
constexpr std::size_t kMostQueued = 256;
constexpr double kMostReach = 50.0;
std::mutex queue_mutex;
std::vector<Question> questions;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;

double number(const sdk::Value* map, const char* name, double fallback) {
  const sdk::Value* field = map ? integration::script_field(*map, name) : nullptr;
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

// A point { x, y, z } of the question; false when it has no finite one.
bool point_of(const sdk::Value& request, const char* name, float out[3]) {
  const sdk::Value* point = integration::script_field(request, name);
  const char* axes[3] = {"x", "y", "z"};
  for (int axis = 0; axis < 3; ++axis) {
    const double value = number(point, axes[axis], NAN);
    if (!std::isfinite(value)) {
      return false;
    }
    out[axis] = float(value);
  }
  return true;
}

const char* status_name(Path::Status status) {
  switch (status) {
    case Path::Status::Found:
      return "found";
    case Path::Status::NoNavmesh:
      return "noNavmesh";
    case Path::Status::StartOff:
      return "startOff";
    case Path::Status::GoalOff:
      return "goalOff";
    default:
      return "failed";
  }
}

sdk::Value point_value(const float* point) {
  return sdk::Value(sdk::Value::Array{sdk::Value(double(point[0])), sdk::Value(double(point[1])),
                                      sdk::Value(double(point[2]))});
}

// The query object's constructor and its set-up on a navmesh (query, navmesh, node count).
constexpr char kQueryConstruct[] = "48 89 5C 24 08 57 48 83 EC 20 33 FF C7 41 64 00";
constexpr char kQueryInit[] = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 89 11 41 8B";
// The sliced path search: start (query, start polygon, goal polygon, start, goal, filter, limit
// as float bits: -1 none), steps (query, most iterations, done), end (query, polygons, count,
// unused, the point it ended on).
constexpr char kInitSliced[] =
    "48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 48 89 78 20 41 54 41 56 41 57 48 83 EC 20 8B "
    "F2 4C";
constexpr char kUpdateSliced[] =
    "48 89 5C 24 18 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC A0 00 "
    "00 00";
constexpr char kFinalizeSliced[] =
    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 41 56 48 83 EC 20 33 FF C7 41 64 00 00 80 BF "
    "4C 8D 49 08 41 89 38 4D 8B F0";
// The polygons as points (query, start, end, polygons, count, points, point flags, point
// polygons, point count, most points, options).
constexpr char kStraightPath[] =
    "48 8B C4 55 53 56 57 41 54 41 55 41 56 41 57 48 8D A8 18 FF FF FF 48 81 EC A8 01 00 00 0F 29 "
    "70 A8 0F 29 78 98 44 0F 29";

constexpr std::size_t kQueryBytes = 184;
constexpr int kQueryNodes = 8192;
constexpr int kMostPolygons = 2048;
constexpr int kStepIterations = 2048;
constexpr int kMostSteps = 32;
constexpr std::uint32_t kFailure = 0x80000000u;
constexpr std::uint32_t kSuccess = 0x40000000u;
constexpr std::uint32_t kInProgress = 0x20000000u;
// -1.0f: the search has no length limit.
constexpr std::uint32_t kNoLimit = 0xBF800000u;

using Construct = void*(__fastcall*)(void* query);
using Init = std::uint32_t(__fastcall*)(void* query, void* navmesh, int nodes);
using InitSliced = std::uint32_t(__fastcall*)(void* query, std::uint32_t start, std::uint32_t goal,
                                              const float* from, const float* to,
                                              const void* filter, std::uint32_t limit);
using UpdateSliced = std::uint32_t(__fastcall*)(void* query, int iterations, int* done);
using FinalizeSliced = std::uint32_t(__fastcall*)(void* query, std::uint32_t* polygons, int* count,
                                                  void* unused, float* end);
using StraightPath = std::uint32_t(__fastcall*)(void* query, const float* from, const float* to,
                                                const std::uint32_t* polygons, int count,
                                                float* points, std::uint8_t* flags,
                                                std::uint32_t* refs, int* point_count, int most,
                                                int options);

// The query's agent id (an entity id; the search asks it about each step).
constexpr std::size_t kQueryAgent = 128;
// The game's lookup of an agent's object by its id (its kind in the top byte), which the search
// calls first, and the id KCDS's own goal check answers to: a kind the game has none of.
constexpr char kAgentLookup[] = "48 89 54 24 10 48 83 EC 28 48 C1 EA 38 48 83 FA 0F 75 21";
constexpr std::uint64_t kOwnAgent = 0x4B00000000000001ull;

Logger log_line = nullptr;
Construct construct = nullptr;
Init init = nullptr;
InitSliced init_sliced = nullptr;
UpdateSliced update_sliced = nullptr;
FinalizeSliced finalize_sliced = nullptr;
StraightPath straight_path = nullptr;

// KCDS's own query object, set up on `query_navmesh`.
alignas(16) std::uint8_t query[kQueryBytes];
bool constructed = false;
void* query_navmesh = nullptr;

// The goal of the search that runs now. The search asks its agent, for each polygon it takes,
// whether that polygon's entry point is at the goal (the first slot of the agent's object). A
// polygon is entered on its edge, and the goal lies in its polygon, which is convex: a step from
// the entry point towards the goal stays in the polygon only when it is the goal's.
struct Goal {
  void* navmesh = nullptr;
  float point[3] = {};
  std::uint32_t polygon = 0;
};
Goal goal_now;
constexpr float kGoalStep = 0.05f;

bool __fastcall reached(void* self, const float* entry) {
  (void)self;
  const float to[3] = {goal_now.point[0] - entry[0], goal_now.point[1] - entry[1],
                       goal_now.point[2] - entry[2]};
  const float length = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
  if (length <= kGoalStep) {
    return true;
  }
  const float inside[3] = {entry[0] + to[0] * kGoalStep / length,
                           entry[1] + to[1] * kGoalStep / length,
                           entry[2] + to[2] * kGoalStep / length};
  const float around[3] = {0.1f, 0.1f, 0.5f};
  float point[3] = {};
  return navmesh_holes::nearest_polygon(goal_now.navmesh, inside, around, point) ==
         goal_now.polygon;
}

// The goal check as the game's agent objects look: a pointer to their function table.
using Reached = bool(__fastcall*)(void* self, const float* entry);
const Reached kGoalCheckTable[1] = {&reached};
const Reached* goal_check = kGoalCheckTable;

using AgentLookup = void*(__fastcall*)(void* agents, std::uint64_t id);
AgentLookup original_lookup = nullptr;

void* __fastcall hooked_lookup(void* agents, std::uint64_t id) {
  if (id == kOwnAgent) {
    return &goal_check;
  }
  return original_lookup(agents, id);
}

bool ready_query(void* navmesh) {
  if (!constructed) {
    construct(query);
    constructed = true;
  }
  if (query_navmesh != navmesh) {
    if ((init(query, navmesh, kQueryNodes) & kSuccess) == 0) {
      query_navmesh = nullptr;
      return false;
    }
    query_navmesh = navmesh;
  }
  return true;
}

}  // namespace

bool install(Logger log) {
  log_line = log;
  void* lookup = resolve_unique("NavmeshAgentLookup", kAgentLookup);
  const bool hooked = lookup &&
                      MH_CreateHook(lookup, reinterpret_cast<void*>(&hooked_lookup),
                                    reinterpret_cast<void**>(&original_lookup)) == MH_OK &&
                      MH_EnableHook(lookup) == MH_OK;
  construct = reinterpret_cast<Construct>(resolve_unique("NavmeshQueryConstruct", kQueryConstruct));
  init = reinterpret_cast<Init>(resolve_unique("NavmeshQueryInit", kQueryInit));
  init_sliced = reinterpret_cast<InitSliced>(resolve_unique("NavmeshInitSliced", kInitSliced));
  update_sliced =
      reinterpret_cast<UpdateSliced>(resolve_unique("NavmeshUpdateSliced", kUpdateSliced));
  finalize_sliced =
      reinterpret_cast<FinalizeSliced>(resolve_unique("NavmeshFinalizeSliced", kFinalizeSliced));
  straight_path =
      reinterpret_cast<StraightPath>(resolve_unique("NavmeshStraightPath", kStraightPath));
  const bool found = hooked && construct && init && init_sliced && update_sliced &&
                     finalize_sliced && straight_path;
  if (log_line) {
    log_line("NAVMESH paths %s", found ? "installed" : "unavailable");
  }
  if (!found) {
    construct = nullptr;
  }
  return found;
}

Path find(const float from[3], const float to[3]) {
  Path path;
  const std::vector<void*> navmeshes = navmesh_holes::known_navmeshes();
  alignas(16) std::uint8_t filter[navmesh_holes::kFilterBytes];
  if (!construct || navmeshes.empty() || !navmesh_holes::walk_filter(filter)) {
    path.status = Path::Status::NoNavmesh;
    return path;
  }
  // A path that is not found says where and with what status, the first few times.
  static int failures_told = 0;
  const auto failed = [&path, &navmeshes](Path::Status why, const char* stage,
                                          std::uint32_t status) {
    path.status = why;
    if (log_line && failures_told < 8) {
      ++failures_told;
      log_line("NAVMESH path not found: %s (status 0x%08X, %zu navmesh(es))", stage, status,
               navmeshes.size());
    }
    return path;
  };
  // The first navmesh both the start and the goal are on (a level may have several, one per
  // agent size).
  const float reach[3] = {kNearReach, kNearReach, kNearReach * 2.0f};
  float start[3] = {};
  float goal[3] = {};
  void* navmesh = nullptr;
  std::uint32_t start_polygon = 0;
  std::uint32_t goal_polygon = 0;
  bool start_found = false;
  for (void* candidate : navmeshes) {
    start_polygon = navmesh_holes::nearest_polygon(candidate, from, reach, start);
    if (start_polygon == 0) {
      continue;
    }
    start_found = true;
    goal_polygon = navmesh_holes::nearest_polygon(candidate, to, reach, goal);
    if (goal_polygon != 0) {
      navmesh = candidate;
      break;
    }
  }
  if (!start_found) {
    return failed(Path::Status::StartOff, "the start is off the navmesh", 0);
  }
  if (!navmesh) {
    return failed(Path::Status::GoalOff, "the goal is off the start's navmesh", 0);
  }
  if (!ready_query(navmesh)) {
    return failed(Path::Status::NoNavmesh, "query set-up", 0);
  }
  *reinterpret_cast<std::uint64_t*>(query + kQueryAgent) = kOwnAgent;
  goal_now.navmesh = navmesh;
  std::copy(goal, goal + 3, goal_now.point);
  goal_now.polygon = goal_polygon;
  std::uint32_t status =
      init_sliced(query, start_polygon, goal_polygon, start, goal, filter, kNoLimit);
  for (int step = 0; step < kMostSteps && (status & kInProgress) != 0; ++step) {
    int done = 0;
    status = update_sliced(query, kStepIterations, &done);
  }
  if ((status & kFailure) != 0 || (status & kInProgress) != 0) {
    return failed(Path::Status::Failed, "search", status);
  }
  static std::array<std::uint32_t, kMostPolygons> polygons;
  int count = 0;
  float end[3] = {};
  status = finalize_sliced(query, polygons.data(), &count, nullptr, end);
  if ((status & kSuccess) == 0 || count <= 0) {
    return failed(Path::Status::Failed, "finalize", status);
  }
  path.reaches = polygons[std::size_t(count - 1)] == goal_polygon;
  // The way ends at the goal's point when it reaches it, else where the search ended.
  const float* last = path.reaches ? goal : end;
  static std::array<float, kMostPoints * 3> points;
  static std::array<std::uint32_t, kMostPoints> refs;
  int point_count = 0;
  status = straight_path(query, start, last, polygons.data(), count, points.data(), nullptr,
                         refs.data(), &point_count, int(kMostPoints), 0);
  if ((status & kSuccess) == 0 || point_count <= 0) {
    return failed(Path::Status::Failed, "points", status);
  }
  path.points.assign(points.begin(), points.begin() + std::ptrdiff_t(point_count) * 3);
  path.status = Path::Status::Found;
  return path;
}

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  const std::string asked = request ? integration::script_operation(*request) : std::string();
  if (!id || id->tag() != sdk::Tag::Int ||
      (asked != "localNavmeshPath" && asked != "localNavmeshNearest")) {
    return false;
  }
  Question question;
  question.id = id->as_int();
  question.nearest = asked == "localNavmeshNearest";
  bool usable = false;
  if (question.nearest) {
    usable = point_of(*request, "at", question.from);
    const double reach = number(request, "reach", kNearReach);
    question.reach = reach > 0.0 && reach <= kMostReach ? float(reach) : kNearReach;
  } else {
    usable = point_of(*request, "from", question.from) && point_of(*request, "to", question.to);
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (usable && questions.size() < kMostQueued) {
    questions.push_back(question);
  } else {
    replies.emplace_back(question.id, sdk::Value());
  }
  return true;
}

void frame(bool in_world) {
  std::vector<Question> asked;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    asked.swap(questions);
  }
  for (const Question& question : asked) {
    sdk::Value value;
    if (in_world && question.nearest) {
      const float reach[3] = {question.reach, question.reach, question.reach * 2.0f};
      for (void* navmesh : navmesh_holes::known_navmeshes()) {
        float point[3] = {};
        if (navmesh_holes::nearest_polygon(navmesh, question.from, reach, point) != 0) {
          value = point_value(point);
          break;
        }
      }
    } else if (in_world) {
      const Path path = find(question.from, question.to);
      sdk::Value::Array points;
      for (std::size_t at = 0; at + 2 < path.points.size(); at += 3) {
        points.push_back(point_value(&path.points[at]));
      }
      value = sdk::Value(sdk::Value::Map{
          {"status", sdk::Value(std::string(status_name(path.status)))},
          {"reaches", sdk::Value(path.reaches)},
          {"points", sdk::Value(std::move(points))},
      });
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    replies.emplace_back(question.id, std::move(value));
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(replies);
  }
  for (auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
}

void reset() {
  // The node pool stays; the next world's navmesh sets the query up again.
  query_navmesh = nullptr;
}

}  // namespace kcds::runtime::world::navmesh_paths
