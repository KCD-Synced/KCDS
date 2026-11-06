#include "local_prefabs.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "game_prefabs.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::prefabs::local {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// Orders kept between two frames at most (an editor's undo of a large selection fits).
constexpr std::size_t kMaxQueued = 4096;

enum class Kind { Spawn, Move, Destroy };

struct Order {
  Kind kind = Kind::Spawn;
  std::uint32_t id = 0;
  std::string template_guid;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
};

struct Placed {
  std::string template_guid;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
};

std::mutex queue_mutex;
std::vector<Order> queued;
// The pieces requests (id, template) and their answers.
constexpr std::size_t kMaxPieces = 64;
std::vector<std::pair<std::int64_t, std::string>> piece_requests;
std::vector<std::pair<std::int64_t, sdk::Value>> piece_replies;
// What stands now, by id (engine thread).
std::map<std::uint32_t, Placed> placed;

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

std::string text(const sdk::Value& value, const char* name) {
  const sdk::Value* field = integration::script_field(value, name);
  return field && field->tag() == sdk::Tag::Str ? field->as_str() : std::string();
}

std::array<float, 3> point(const sdk::Value* value) {
  std::array<float, 3> out{};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {float(number(*value, "x", 0)), float(number(*value, "y", 0)),
           float(number(*value, "z", 0))};
  }
  return out;
}

std::array<float, 4> turn(const sdk::Value* value) {
  std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {float(number(*value, "x", 0)), float(number(*value, "y", 0)),
           float(number(*value, "z", 0)), float(number(*value, "w", 1))};
  }
  return out;
}

std::string name_of(std::uint32_t id) {
  return "kcds_local_prefab_" + std::to_string(id);
}

void build(std::uint32_t id, const Placed& prefab) {
  spawn(name_of(id), prefab.template_guid, prefab.position.data(), prefab.rotation.data());
}

void apply(const Order& order) {
  if (order.kind == Kind::Destroy) {
    remove(name_of(order.id));
    placed.erase(order.id);
    return;
  }
  Placed& prefab = placed[order.id];
  if (order.kind == Kind::Spawn) {
    prefab.template_guid = order.template_guid;
  }
  prefab.position = order.position;
  prefab.rotation = order.rotation;
  if (prefab.template_guid.empty()) {
    placed.erase(order.id);
    return;
  }
  // spawn() takes away what stood under that name first: a move builds it again there.
  remove(name_of(order.id));
  build(order.id, prefab);
}

// A pieces request through the sync channel; true when the message was one.
bool queue_pieces_request(const sdk::Value& message) {
  if (integration::script_operation(message) != "syncRequest") {
    return false;
  }
  const sdk::Value* id = integration::script_field(message, "id");
  const sdk::Value* request = integration::script_field(message, "request");
  if (!id || id->tag() != sdk::Tag::Int || !request ||
      integration::script_operation(*request) != "localPrefabPieces") {
    return false;
  }
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (piece_requests.size() < kMaxQueued) {
    piece_requests.emplace_back(id->as_int(), text(*request, "template"));
  }
  return true;
}

sdk::Value pieces_value(const std::string& guid) {
  sdk::Value::Array list;
  for (const Piece& piece : pieces(guid, kMaxPieces)) {
    list.push_back(sdk::Value(sdk::Value::Map{
        {"mesh", sdk::Value(piece.mesh)},
        {"offset", sdk::Value(sdk::Value::Array{sdk::Value(double(piece.position[0])),
                                                sdk::Value(double(piece.position[1])),
                                                sdk::Value(double(piece.position[2]))})},
        {"rotation",
         sdk::Value(sdk::Value::Array{
             sdk::Value(double(piece.rotation[0])), sdk::Value(double(piece.rotation[1])),
             sdk::Value(double(piece.rotation[2])), sdk::Value(double(piece.rotation[3]))})},
    }));
  }
  return sdk::Value(std::move(list));
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (queue_pieces_request(message)) {
    return true;
  }
  if (integration::script_operation(message) != "localPrefab") {
    return false;
  }
  const std::string kind = text(message, "kind");
  Order order;
  if (kind == "spawn") {
    order.kind = Kind::Spawn;
  } else if (kind == "move") {
    order.kind = Kind::Move;
  } else if (kind == "destroy") {
    order.kind = Kind::Destroy;
  } else {
    return true;
  }
  order.id = std::uint32_t(number(message, "id", 0));
  order.template_guid = text(message, "template");
  order.position = point(integration::script_field(message, "position"));
  order.rotation = turn(integration::script_field(message, "rotation"));
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(std::move(order));
  }
  return true;
}

void frame(bool in_world) {
  std::vector<std::pair<std::int64_t, std::string>> asked;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    asked.swap(piece_requests);
  }
  for (const auto& [id, guid] : asked) {
    sdk::Value value = in_world ? pieces_value(guid) : sdk::Value();
    std::lock_guard<std::mutex> lock(queue_mutex);
    piece_replies.emplace_back(id, std::move(value));
  }
  if (!in_world) {
    return;
  }
  std::vector<Order> orders;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    orders.swap(queued);
  }
  // A move is "take away what stands under the name, build it again", and the game takes an
  // entity away only at the end of the frame: a second move of the same prefab in one frame found
  // the old one and left the first new one standing (World Architect's arrow drag, live
  // two houses). Only a prefab's last move of a frame is made.
  for (std::size_t index = 0; index < orders.size(); ++index) {
    const Order& order = orders[index];
    if (order.kind == Kind::Move) {
      const bool moved_again = std::any_of(
          orders.begin() + std::ptrdiff_t(index) + 1, orders.end(),
          [&](const Order& later) { return later.id == order.id && later.kind == Kind::Move; });
      if (moved_again) {
        continue;
      }
    }
    apply(order);
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(piece_replies);
  }
  for (const auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{
        {"op", sdk::Value(std::string("syncReply"))}, {"id", sdk::Value(id)}, {"value", value}}));
  }
}

void reset() {
  placed.clear();
  std::lock_guard<std::mutex> lock(queue_mutex);
  queued.clear();
}

}  // namespace kcds::runtime::prefabs::local
