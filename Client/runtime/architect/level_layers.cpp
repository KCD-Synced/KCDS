#include "level_layers.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../level/layer_catalog.hpp"
#include "../level/story_layers.hpp"
#include "kcd2s/integration/script_channel.hpp"
#include "local_mode.hpp"

namespace kcds::runtime::architect::level_layers {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace layers = kcds::runtime::level::layers;

constexpr std::size_t kMostLayers = 64;
constexpr std::size_t kMostQueued = 16;
constexpr double kMostRadius = 400.0;

enum class Kind { List, Set, Restore };

struct Request {
  std::int64_t id = 0;
  Kind kind = Kind::List;
  std::string map;
  float center[3] = {};
  float radius = 0.0f;
  std::size_t most = kMostLayers;
  std::string name;
  bool on = false;
};

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

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

sdk::Value list_of(const Request& request) {
  sdk::Value::Array list;
  for (const auto& layer :
       layers::around(request.map, request.center, request.radius, request.most)) {
    list.push_back(sdk::Value(sdk::Value::Map{
        {"id", sdk::Value(std::int64_t(layer.id))},
        {"name", sdk::Value(layer.name)},
        {"short", sdk::Value(layers::short_layer_name(layer.name))},
        {"path", sdk::Value(layer.path)},
        {"on", sdk::Value(layer.on)},
        {"brushes", sdk::Value(std::int64_t(layer.brushes))},
        {"shown", sdk::Value(std::int64_t(layer.shown))},
        {"entities", sdk::Value(std::int64_t(layer.entities))},
        {"p", sdk::Value(sdk::Value::Array{sdk::Value(double(layer.at[0])),
                                           sdk::Value(double(layer.at[1])),
                                           sdk::Value(double(layer.at[2]))})},
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
      (asked_for != "localLevelLayers" && asked_for != "localLevelLayer" &&
       asked_for != "localLevelLayersRestore")) {
    return false;
  }
  Request asked;
  asked.id = id->as_int();
  if (asked_for == "localLevelLayers") {
    asked.kind = Kind::List;
    asked.map = text(*request, "map");
    const sdk::Value* center = integration::script_field(*request, "center");
    asked.center[0] = float(number(center, "x", 0));
    asked.center[1] = float(number(center, "y", 0));
    asked.center[2] = float(number(center, "z", 0));
    const double radius = number(request, "radius", 0);
    asked.radius = radius > 0.0 && radius <= kMostRadius ? float(radius) : 0.0f;
    const double most = number(request, "most", double(kMostLayers));
    asked.most = most >= 1.0 && most <= double(kMostLayers) ? std::size_t(most) : kMostLayers;
  } else if (asked_for == "localLevelLayer") {
    asked.kind = Kind::Set;
    asked.name = text(*request, "name");
    const sdk::Value* on = integration::script_field(*request, "on");
    asked.on = on && on->tag() == sdk::Tag::Bool && on->as_bool();
  } else {
    asked.kind = Kind::Restore;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (requests.size() < kMostQueued) {
    requests.push_back(std::move(asked));
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
      if (request.kind == Kind::List && request.radius > 0.0f && !request.map.empty()) {
        value = list_of(request);
      } else if (request.kind == Kind::Set) {
        value = sdk::Value(layers::set(request.name, request.on));
      } else if (request.kind == Kind::Restore) {
        layers::restore_all();
        value = sdk::Value(true);
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

}  // namespace kcds::runtime::architect::level_layers
