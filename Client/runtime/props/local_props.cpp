#include "local_props.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"
#include "prop_entity.hpp"

namespace kcds::runtime::props::local {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// A static body takes the mass its mesh's collision gives it.
constexpr float kMeshMass = -1.0f;
constexpr std::size_t kMaxQueued = 4096;
// A mesh path: the game's objects only, of a sane length.
constexpr std::size_t kMaxMesh = 260;
// The sizes a prop may take.
constexpr double kMinScale = 0.1;
constexpr double kMaxScale = 10.0;

enum class Kind { Set, Remove };

struct Prop {
  std::string mesh;
  // A material drawn in place of the mesh's own (a game path without .mtl); empty: its own.
  std::string material;
  std::array<float, 3> position{};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
  float scale = 1.0f;
};

struct Order {
  Kind kind = Kind::Set;
  std::uint32_t id = 0;
  Prop prop;
};

std::mutex queue_mutex;
std::vector<Order> queued;
// localPropBounds questions (id, mesh) and their answers, sent at the next pump.
std::vector<std::pair<std::int64_t, std::string>> asked_bounds;
std::vector<std::pair<std::int64_t, sdk::Value>> bound_replies;
// What stands now, by id (engine thread).
std::map<std::uint32_t, Prop> standing;

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

std::array<float, 3> vector(const sdk::Value* value) {
  std::array<float, 3> out{};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {float(number(*value, "x", 0)), float(number(*value, "y", 0)),
           float(number(*value, "z", 0))};
  }
  return out;
}

std::array<float, 4> quaternion(const sdk::Value* value) {
  std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {float(number(*value, "x", 0)), float(number(*value, "y", 0)),
           float(number(*value, "z", 0)), float(number(*value, "w", 1))};
  }
  return out;
}

// A mesh path the game may load: under objects/, a .cgf, nothing that climbs out.
bool usable_mesh(const std::string& mesh) {
  const bool sane = !mesh.empty() && mesh.size() <= kMaxMesh && mesh.rfind("objects/", 0) == 0 &&
                    mesh.find("..") == std::string::npos;
  return sane && mesh.size() > 4 && mesh.compare(mesh.size() - 4, 4, ".cgf") == 0;
}

std::string name_of(std::uint32_t id) {
  return "kcds_local_prop_" + std::to_string(id);
}

void build(std::uint32_t id, const Prop& prop) {
  const std::string name = name_of(id);
  entity::remove(name);
  void* body = entity::spawn_solid_mesh(name, prop.position.data(), prop.mesh);
  if (!body) {
    return;
  }
  entity::rotate(body, prop.rotation.data());
  if (!prop.material.empty()) {
    entity::draw_with_material(body, prop.material);
  }
  if (prop.scale != 1.0f) {
    const float scale[3] = {prop.scale, prop.scale, prop.scale};
    entity::set_scale(body, scale);
  }
  entity::physicalize(body, entity::Physics::Static, kMeshMass);
}

void apply(const Order& order) {
  if (order.kind == Kind::Remove) {
    entity::remove(name_of(order.id));
    standing.erase(order.id);
    return;
  }
  const auto found = standing.find(order.id);
  // The same mesh at the same size moves; another size is built again (its body takes the size).
  const bool same = found != standing.end() && found->second.mesh == order.prop.mesh &&
                    found->second.scale == order.prop.scale &&
                    found->second.material == order.prop.material;
  void* existing = same ? entity::find(name_of(order.id)) : nullptr;
  standing[order.id] = order.prop;
  if (existing) {
    entity::place_in_world(existing, order.prop.position.data(), order.prop.rotation.data());
    return;
  }
  build(order.id, order.prop);
}

// A standing prop's box in its own frame, as { min: [x, y, z], max: [x, y, z] }; null when no prop
// of that mesh stands (yet).
sdk::Value bounds_of(const std::string& mesh) {
  for (const auto& [id, prop] : standing) {
    if (prop.mesh != mesh) {
      continue;
    }
    float min[3] = {};
    float max[3] = {};
    if (!entity::local_bounds(entity::find(name_of(id)), min, max)) {
      continue;
    }
    const auto list = [](const float values[3]) {
      return sdk::Value(sdk::Value::Array{sdk::Value(double(values[0])),
                                          sdk::Value(double(values[1])),
                                          sdk::Value(double(values[2]))});
    };
    return sdk::Value(sdk::Value::Map{{"min", list(min)}, {"max", list(max)}});
  }
  return sdk::Value();
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) == "syncRequest") {
    const sdk::Value* id = integration::script_field(message, "id");
    const sdk::Value* request = integration::script_field(message, "request");
    if (!id || id->tag() != sdk::Tag::Int || !request ||
        integration::script_operation(*request) != "localPropBounds") {
      return false;
    }
    std::lock_guard<std::mutex> lock(queue_mutex);
    asked_bounds.emplace_back(id->as_int(), text(*request, "mesh"));
    return true;
  }
  if (integration::script_operation(message) != "localProp") {
    return false;
  }
  const std::string kind = text(message, "kind");
  Order order;
  if (kind == "set") {
    order.kind = Kind::Set;
    order.prop.mesh = text(message, "mesh");
    std::transform(order.prop.mesh.begin(), order.prop.mesh.end(), order.prop.mesh.begin(),
                   [](char c) { return c == '\\' ? '/' : char(std::tolower(unsigned(c))); });
    if (!usable_mesh(order.prop.mesh)) {
      return true;
    }
    order.prop.material = text(message, "material");
    if (order.prop.material.size() > kMaxMesh ||
        order.prop.material.find("..") != std::string::npos) {
      order.prop.material.clear();
    }
  } else if (kind == "remove") {
    order.kind = Kind::Remove;
  } else {
    return true;
  }
  order.id = std::uint32_t(number(message, "id", 0));
  order.prop.position = vector(integration::script_field(message, "position"));
  order.prop.rotation = quaternion(integration::script_field(message, "rotation"));
  const double scale = number(message, "scale", 1.0);
  order.prop.scale = scale >= kMinScale && scale <= kMaxScale ? float(scale) : 1.0f;
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(std::move(order));
  }
  return true;
}

void frame(bool in_world) {
  std::vector<std::pair<std::int64_t, std::string>> questions;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    questions.swap(asked_bounds);
  }
  for (const auto& [id, mesh] : questions) {
    sdk::Value answer = in_world ? bounds_of(mesh) : sdk::Value();
    std::lock_guard<std::mutex> lock(queue_mutex);
    bound_replies.emplace_back(id, std::move(answer));
  }
  if (!in_world) {
    return;
  }
  std::vector<Order> orders;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    orders.swap(queued);
  }
  for (std::size_t index = 0; index < orders.size(); ++index) {
    const Order& order = orders[index];
    const bool set_again = order.kind == Kind::Set &&
                           std::any_of(orders.begin() + std::ptrdiff_t(index) + 1, orders.end(),
                                       [&](const Order& later) { return later.id == order.id; });
    if (!set_again) {
      apply(order);
    }
  }
}

void pump(const std::function<bool(const sdk::Value&)>& send) {
  std::vector<std::pair<std::int64_t, sdk::Value>> taken;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    taken.swap(bound_replies);
  }
  for (auto& [id, value] : taken) {
    send(sdk::Value(sdk::Value::Map{{"op", sdk::Value(std::string("syncReply"))},
                                    {"id", sdk::Value(id)},
                                    {"value", std::move(value)}}));
  }
}

void reset() {
  standing.clear();
  std::lock_guard<std::mutex> lock(queue_mutex);
  queued.clear();
}

}  // namespace kcds::runtime::props::local
