#include "navmesh_bake.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "../core/log.hpp"
#include "../maps/custom_maps.hpp"
#include "../maps/custom_navmesh.hpp"
#include "../terrain/terrain_edits.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::navmesh_bake {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// Blocked areas one request may name.
constexpr std::size_t kMostBlocked = 4096;
// How far inside the map's far edge its last heights are read, metres.
constexpr float kEdgeInset = 1.5f;

struct Request {
  std::int64_t id = 0;
  std::vector<maps::custom::Blocked> blocked;
};

std::mutex queue_mutex;
std::vector<Request> requests;
std::vector<std::pair<std::int64_t, sdk::Value>> replies;

double number(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  if (field && field->tag() == sdk::Tag::Int) {
    return double(field->as_int());
  }
  if (field && field->tag() == sdk::Tag::Double) {
    return field->as_double();
  }
  return NAN;
}

sdk::Value bake(const std::vector<maps::custom::Blocked>& blocked) {
  const std::string level = local_mode::world().map;
  const int size = maps::custom::map_size(level);
  if (!maps::custom::is_custom(level) || size <= 0) {
    return sdk::Value();
  }
  // The ground has no sample on its far edge (x or y = size): those corners take the last row's.
  const float inside = float(size) - kEdgeInset;
  const auto height_at = [inside](float x, float y, float& height) {
    return terrain::height_at(std::min(x, inside), std::min(y, inside), height);
  };
  const maps::custom::NavmeshFiles files = maps::custom::make_navmesh(size, height_at, blocked);
  const bool kept = maps::custom::keep_next_navmesh(level, files.index, files.tiles);
  kcds::game_client::log("ARCHITECT NAVMESH %s: %d tiles, %d walkable squares, %d left out, %s",
                         level.c_str(), files.tile_count, files.walkable, files.left_out,
                         kept ? "kept for the next start" : "NOT written");
  if (!kept) {
    return sdk::Value();
  }
  return sdk::Value(sdk::Value::Map{{"tiles", sdk::Value(std::int64_t(files.tile_count))},
                                    {"walkable", sdk::Value(std::int64_t(files.walkable))},
                                    {"leftOut", sdk::Value(std::int64_t(files.left_out))}});
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      integration::script_operation(*request) != "localNavmeshBake") {
    return false;
  }
  Request asked;
  asked.id = id->as_int();
  const sdk::Value* blocked = integration::script_field(*request, "blocked");
  if (blocked && blocked->tag() == sdk::Tag::Array) {
    for (const sdk::Value& area : blocked->as_array()) {
      if (asked.blocked.size() >= kMostBlocked) {
        break;
      }
      maps::custom::Blocked box;
      box.min_x = float(number(area, "minX"));
      box.min_y = float(number(area, "minY"));
      box.max_x = float(number(area, "maxX"));
      box.max_y = float(number(area, "maxY"));
      if (std::isfinite(box.min_x) && std::isfinite(box.min_y) && std::isfinite(box.max_x) &&
          std::isfinite(box.max_y)) {
        asked.blocked.push_back(box);
      }
    }
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  requests.push_back(std::move(asked));
  return true;
}

void frame(bool in_world) {
  if (!in_world) {
    return;
  }
  std::vector<Request> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(requests);
  }
  for (const Request& request : taken) {
    sdk::Value answer = local_mode::enabled() ? bake(request.blocked) : sdk::Value();
    std::lock_guard<std::mutex> lock(queue_mutex);
    replies.emplace_back(request.id, std::move(answer));
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

}  // namespace kcds::runtime::architect::navmesh_bake
