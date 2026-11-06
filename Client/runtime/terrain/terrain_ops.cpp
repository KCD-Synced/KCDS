#include "terrain_ops.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "kcd2s/integration/script_channel.hpp"
#include "merged_grass.hpp"
#include "terrain_edits.hpp"

namespace kcds::runtime::terrain::ops {
namespace {

namespace sdk = kcd2s::sdk;
namespace integration = kcd2s::integration;

// Orders kept between two frames at most (a brush stroke's dabs fit).
constexpr std::size_t kMaxQueued = 1024;

enum class Kind : std::uint8_t { Edit, Revert, HideGrass, ShowGrass };

struct Order {
  Kind kind = Kind::Edit;
  std::uint32_t id = 0;
  Edit edit;
};

std::mutex queue_mutex;
std::vector<Order> queued;

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

}  // namespace

bool handle_runtime_message(const sdk::Value& message) {
  const std::string operation = integration::script_operation(message);
  if (operation == "architectGrass") {
    // World Architect's nature brush: the level's grass carpet hidden in a circle, or back.
    Order order;
    order.id = std::uint32_t(number(message, "id", 0));
    order.kind = text(message, "kind") == "show" ? Kind::ShowGrass : Kind::HideGrass;
    order.edit.x = float(number(message, "x", 0));
    order.edit.y = float(number(message, "y", 0));
    order.edit.radius = float(number(message, "radius", 0));
    std::lock_guard<std::mutex> lock(queue_mutex);
    if (queued.size() < kMaxQueued && order.id != 0) {
      queued.push_back(order);
    }
    return true;
  }
  if (operation != "localTerrain") {
    return false;
  }
  Order order;
  order.id = std::uint32_t(number(message, "id", 0));
  const std::string kind = text(message, "kind");
  if (kind == "revert") {
    order.kind = Kind::Revert;
  } else if (kind == "edit") {
    const std::string shape = text(message, "shape");
    if (shape == "flatten") {
      order.edit.shape = Shape::Flatten;
    } else if (shape == "raise") {
      order.edit.shape = Shape::Raise;
    } else if (shape == "paint") {
      order.edit.shape = Shape::Paint;
    } else {
      return true;
    }
    order.edit.x = float(number(message, "x", 0));
    order.edit.y = float(number(message, "y", 0));
    order.edit.radius = float(number(message, "radius", 1));
    order.edit.falloff = float(number(message, "falloff", 0));
    order.edit.value = float(number(message, "value", 0));
  } else {
    return true;
  }
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
    if (order.kind == Kind::Revert) {
      revert(order.id);
    } else if (order.kind == Kind::HideGrass) {
      grass::hide(order.id, order.edit.x, order.edit.y, order.edit.radius);
    } else if (order.kind == Kind::ShowGrass) {
      grass::show(order.id);
    } else {
      apply(order.id, order.edit);
    }
  }
  grass::frame();
}

void reset() {
  {
    std::lock_guard<std::mutex> lock(queue_mutex);
    queued.clear();
  }
  grass::reset();
}

}  // namespace kcds::runtime::terrain::ops
