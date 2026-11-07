#include "level_plants.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "../level/level_edits.hpp"
#include "../physics/local_world.hpp"
#include "../terrain/merged_grass.hpp"
#include "../vegetation/local_vegetation.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::level_plants {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

constexpr std::size_t kMostPlants = 256;
constexpr std::size_t kMostQueued = 16;
constexpr double kMostRadius = 40.0;

struct Request {
  std::int64_t id = 0;
  float center[3] = {};
  float radius = 0.0f;
  std::size_t most = 0;
  // A localGroundHeights request: the points whose ground is asked (empty for the plants), and
  // how far up and down from each the ground is looked for.
  std::vector<std::array<float, 3>> points;
  float reach = 40.0f;
  bool heights = false;
  // A localGrassKinds request (center and radius as for the plants).
  bool grass = false;
  // A localLevelBrushes request (center, radius and most as for the plants); with all, the
  // buildings and other fixed meshes too (World Architect's hide_level_named).
  bool brushes = false;
  bool all = false;
  // With hidden, also the brushes their layer hides (the level's other states: a church finished
  // in a later part of the story where this game shows its scaffolding), each marked hidden.
  bool hidden = false;
};

// The parts of mesh paths that are no loose object: they stay where the level built them when the
// ground under them changes.
constexpr const char* kFixedMeshes[] = {"/structures/", "/buildings/", "/roads/",
                                        "/road_",       "/bridges/",   "/walls/",
                                        "/terrain/",    "/fortress/",  "/castle"};

bool loose(const std::string& mesh) {
  std::string lower = mesh;
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](char c) { return char(std::tolower(unsigned(c))); });
  for (const char* part : kFixedMeshes) {
    if (lower.find(part) != std::string::npos) {
      return false;
    }
  }
  return true;
}

// The most points one localGroundHeights question asks for (Export for Blender asks a grid of
// them, a few questions instead of hundreds that each wait a frame).
constexpr std::size_t kMostPoints = 1024;
// How far up and down a point's ground is looked for: by default, and at most (an AI assistant
// asking for the ground under a point without a height, ai_commands.cjs).
constexpr float kGroundReach = 40.0f;
constexpr double kMostReach = 4000.0;

// A terrain-only ray straight down through a heightmap sample (whole metres: the grid, an AI
// assistant's points, a sector's edge) can miss the terrain (every miss of a
// 512 m map was on one, and a centimetre aside hit): such a point is asked again a little aside,
// then to the other side (at the map's far edge).
constexpr float kAside[3][2] = {{0.0f, 0.0f}, {0.013f, 0.017f}, {-0.013f, -0.017f}};

sdk::Value terrain_hit(const std::array<float, 3>& point, float reach) {
  for (const auto& aside : kAside) {
    const float from[3] = {point[0] + aside[0], point[1] + aside[1], point[2] + reach};
    const float to[3] = {point[0] + aside[0], point[1] + aside[1], point[2] - reach};
    const sdk::Value hit = physics::first_hit(from, to, 3);
    if (hit.tag() == sdk::Tag::Map) {
      return hit;
    }
  }
  return sdk::Value();
}

sdk::Value heights_of(const Request& request) {
  sdk::Value::Array found;
  for (const auto& point : request.points) {
    // The terrain only: plants scattered or settled go on the ground, not on the tree tops and
    // rocks a ground ray meets (Add nature put plants 16 m up).
    const sdk::Value hit = terrain_hit(point, request.reach);
    const sdk::Value* place =
        hit.tag() == sdk::Tag::Map ? integration::script_field(hit, "p") : nullptr;
    if (place && place->tag() == sdk::Tag::Array && place->as_array().size() == 3 &&
        place->as_array()[2].tag() == sdk::Tag::Double) {
      found.push_back(sdk::Value(place->as_array()[2].as_double()));
    } else {
      found.push_back(sdk::Value());
    }
  }
  return sdk::Value(std::move(found));
}

std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;

double number(const sdk::Value* value, const char* name, double fallback) {
  const sdk::Value* field = value ? integration::script_field(*value, name) : nullptr;
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

// The grass carpet's kinds around the center, the most common first: [{ kind, mesh, count }].
sdk::Value grass_of(const Request& request) {
  auto kinds = terrain::grass::kinds_near(request.center[0], request.center[1], request.radius);
  std::sort(kinds.begin(), kinds.end(),
            [](const auto& a, const auto& b) { return a.second > b.second; });
  sdk::Value::Array list;
  for (const auto& [kind, count] : kinds) {
    const std::string mesh = vegetation::kind_path(kind);
    if (mesh.empty()) {
      continue;
    }
    list.push_back(sdk::Value(sdk::Value::Map{
        {"kind", sdk::Value(std::int64_t(kind))},
        {"mesh", sdk::Value(mesh)},
        {"count", sdk::Value(std::int64_t(count))},
    }));
  }
  return sdk::Value(std::move(list));
}

sdk::Value brushes_of(const Request& request) {
  sdk::Value::Array list;
  for (const level::LevelBrush& brush : level::brushes_near(request.center, request.radius)) {
    const float x = brush.matrix[3];
    const float y = brush.matrix[7];
    const float z = brush.matrix[11];
    const float away = std::hypot(x - request.center[0], y - request.center[1]);
    const bool shown_or_asked = !brush.hidden || request.hidden;
    if (!shown_or_asked || away > request.radius || (!request.all && !loose(brush.mesh))) {
      continue;
    }
    // Its turn about z and its scale from the matrix's first column (its x axis in the world).
    const double ax = brush.matrix[0];
    const double ay = brush.matrix[4];
    const double az = brush.matrix[8];
    const double turn = std::atan2(ay, ax) * 180.0 / 3.14159265358979323846;
    list.push_back(sdk::Value(sdk::Value::Map{
        {"mesh", sdk::Value(brush.mesh)},
        {"p", sdk::Value(sdk::Value::Array{sdk::Value(double(x)), sdk::Value(double(y)),
                                           sdk::Value(double(z))})},
        {"turn", sdk::Value(std::round(turn * 100.0) / 100.0)},
        {"scale", sdk::Value(std::sqrt(ax * ax + ay * ay + az * az))},
        {"hidden", sdk::Value(brush.hidden)},
    }));
    if (list.size() >= request.most) {
      break;
    }
  }
  return sdk::Value(std::move(list));
}

sdk::Value answer(const Request& request) {
  sdk::Value::Array list;
  for (const level::LevelPlant& plant :
       level::plants_near(request.center, request.radius, request.most)) {
    list.push_back(sdk::Value(sdk::Value::Map{
        {"mesh", sdk::Value(plant.mesh)},
        {"p", sdk::Value(sdk::Value::Array{sdk::Value(double(plant.position[0])),
                                           sdk::Value(double(plant.position[1])),
                                           sdk::Value(double(plant.position[2]))})},
    }));
  }
  return sdk::Value(std::move(list));
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  const std::string asked_for = request ? integration::script_operation(*request) : std::string();
  if (!id || id->tag() != sdk::Tag::Int ||
      (asked_for != "localLevelPlants" && asked_for != "localGroundHeights" &&
       asked_for != "localGrassKinds" && asked_for != "localLevelBrushes")) {
    return false;
  }
  Request asked;
  asked.id = id->as_int();
  asked.grass = asked_for == "localGrassKinds";
  asked.brushes = asked_for == "localLevelBrushes";
  if (const sdk::Value* all = integration::script_field(*request, "all");
      asked.brushes && all && all->tag() == sdk::Tag::Bool) {
    asked.all = all->as_bool();
  }
  if (const sdk::Value* hidden = integration::script_field(*request, "hidden");
      asked.brushes && hidden && hidden->tag() == sdk::Tag::Bool) {
    asked.hidden = hidden->as_bool();
  }
  if (asked_for == "localGroundHeights") {
    asked.heights = true;
    const double reach = number(request, "reach", kGroundReach);
    asked.reach = reach > 0.0 && reach <= kMostReach ? float(reach) : kGroundReach;
    const sdk::Value* points = integration::script_field(*request, "points");
    if (points && points->tag() == sdk::Tag::Array) {
      for (const sdk::Value& point : points->as_array()) {
        if (asked.points.size() >= kMostPoints || point.tag() != sdk::Tag::Array ||
            point.as_array().size() != 3) {
          continue;
        }
        std::array<float, 3> place{};
        bool usable = true;
        for (int axis = 0; axis < 3; ++axis) {
          const sdk::Value& value = point.as_array()[std::size_t(axis)];
          const bool number = value.tag() == sdk::Tag::Double || value.tag() == sdk::Tag::Int;
          const double v = value.tag() == sdk::Tag::Int ? double(value.as_int())
                           : number                     ? value.as_double()
                                                        : 0.0;
          usable = usable && number && std::isfinite(v);
          place[std::size_t(axis)] = float(v);
        }
        if (usable) {
          asked.points.push_back(place);
        }
      }
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (requests.size() < kMostQueued) {
      requests.push_back(std::move(asked));
    } else {
      replies.emplace_back(asked.id, sdk::Value());
    }
    return true;
  }
  const sdk::Value* center = integration::script_field(*request, "center");
  asked.center[0] = float(number(center, "x", 0));
  asked.center[1] = float(number(center, "y", 0));
  asked.center[2] = float(number(center, "z", 0));
  const double radius = number(request, "radius", 0);
  const double most = number(request, "most", double(kMostPlants));
  const bool usable = std::isfinite(asked.center[0]) && std::isfinite(asked.center[1]) &&
                      std::isfinite(asked.center[2]) && radius > 0.0 && radius <= kMostRadius;
  asked.radius = usable ? float(radius) : 0.0f;
  asked.most = most >= 1.0 && most <= double(kMostPlants) ? std::size_t(most) : kMostPlants;
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (requests.size() < kMostQueued) {
    requests.push_back(asked);
  } else {
    replies.emplace_back(asked.id, sdk::Value());
  }
  return true;
}

void frame(bool in_world) {
  std::vector<Request> asked;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    asked.swap(requests);
  }
  for (const Request& request : asked) {
    sdk::Value value;
    if (in_world && local_mode::enabled()) {
      if (request.heights) {
        value = heights_of(request);
      } else if (request.brushes && request.radius > 0.0f) {
        value = brushes_of(request);
      } else if (request.grass && request.radius > 0.0f) {
        value = grass_of(request);
      } else if (request.radius > 0.0f) {
        value = answer(request);
      }
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    replies.emplace_back(request.id, std::move(value));
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

}  // namespace kcds::runtime::architect::level_plants
