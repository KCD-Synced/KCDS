#include "local_collision.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "../props/prop_entity.hpp"
#include "kcd2s/integration/script_channel.hpp"

namespace kcds::runtime::collision::local {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;
namespace entity = kcds::runtime::props::entity;

// The game's cube, 2 m across, its origin in the middle of its bottom face.
constexpr const char* kCube = "objects/special/primitive_cube.cgf";
// The editor material that draws nothing: only the body is there.
constexpr const char* kInvisible = "objects/editor/nodraw";
// A static body takes the mass its mesh's collision gives it.
constexpr float kMeshMass = -1.0f;
// Half sizes a box may have, metres.
constexpr float kMinHalf = 0.01f;
constexpr float kMaxHalf = 100.0f;
constexpr std::size_t kMaxQueued = 4096;

enum class Kind { Set, Remove };

struct Box {
  std::array<float, 3> center{};
  std::array<float, 3> half{0.5f, 0.5f, 0.5f};
  std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f};
};

struct Order {
  Kind kind = Kind::Set;
  std::uint32_t id = 0;
  Box box;
};

std::mutex queue_mutex;
std::vector<Order> queued;
// What stands now, by id (engine thread).
std::map<std::uint32_t, Box> standing;

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

std::array<float, 3> vector(const sdk::Value* value, float fallback) {
  std::array<float, 3> out{fallback, fallback, fallback};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {
        float(number(*value, "x", fallback)),
        float(number(*value, "y", fallback)),
        float(number(*value, "z", fallback)),
    };
  }
  return out;
}

std::array<float, 4> quaternion(const sdk::Value* value) {
  std::array<float, 4> out{0.0f, 0.0f, 0.0f, 1.0f};
  if (value && value->tag() == sdk::Tag::Map) {
    out = {
        float(number(*value, "x", 0)),
        float(number(*value, "y", 0)),
        float(number(*value, "z", 0)),
        float(number(*value, "w", 1)),
    };
  }
  return out;
}

float clamp_half(float value) {
  if (!std::isfinite(value) || value < kMinHalf) {
    return kMinHalf;
  }
  if (value > kMaxHalf) {
    return kMaxHalf;
  }
  return value;
}

std::string name_of(std::uint32_t id) {
  return "kcds_local_collision_" + std::to_string(id);
}

// The cube stands on its bottom face: its origin is the centre less the turned half height.
std::array<float, 3> origin_of(const Box& box) {
  const auto& q = box.rotation;
  const float up[3] = {
      2.0f * (q[0] * q[2] + q[3] * q[1]),
      2.0f * (q[1] * q[2] - q[3] * q[0]),
      1.0f - 2.0f * (q[0] * q[0] + q[1] * q[1]),
  };
  return {
      box.center[0] - up[0] * box.half[2],
      box.center[1] - up[1] * box.half[2],
      box.center[2] - up[2] * box.half[2],
  };
}

void build(std::uint32_t id, const Box& box) {
  const std::string name = name_of(id);
  entity::remove(name);
  const std::array<float, 3> origin = origin_of(box);
  void* body = entity::spawn_solid_mesh(name, origin.data(), kCube);
  if (!body) {
    return;
  }
  entity::rotate(body, box.rotation.data());
  entity::set_scale(body, box.half.data());
  entity::physicalize(body, entity::Physics::Static, kMeshMass);
  entity::draw_with_material(body, kInvisible);
}

void apply(const Order& order) {
  if (order.kind == Kind::Remove) {
    entity::remove(name_of(order.id));
    standing.erase(order.id);
    return;
  }
  standing[order.id] = order.box;
  build(order.id, order.box);
}

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  if (integration::script_operation(message) != "localCollision") {
    return false;
  }
  const std::string kind = text(message, "kind");
  Order order;
  if (kind == "set") {
    order.kind = Kind::Set;
  } else if (kind == "remove") {
    order.kind = Kind::Remove;
  } else {
    return true;
  }
  order.id = std::uint32_t(number(message, "id", 0));
  order.box.center = vector(integration::script_field(message, "center"), 0.0f);
  const std::array<float, 3> half = vector(integration::script_field(message, "half"), 0.5f);
  for (std::size_t i = 0; i < 3; ++i) {
    order.box.half[i] = clamp_half(half[i]);
  }
  order.box.rotation = quaternion(integration::script_field(message, "rotation"));
  std::lock_guard<std::mutex> lock(queue_mutex);
  if (queued.size() < kMaxQueued) {
    queued.push_back(std::move(order));
  }
  return true;
}

void frame(bool in_world) {
  if (!in_world) {
    return;
  }
  std::vector<Order> orders;
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    orders.swap(queued);
  }
  for (const Order& order : orders) {
    apply(order);
  }
}

void reset() {
  standing.clear();
  std::lock_guard<std::mutex> lock(queue_mutex);
  queued.clear();
}

}  // namespace kcds::runtime::collision::local
